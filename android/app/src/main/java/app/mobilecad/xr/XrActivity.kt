package app.mobilecad.xr

import android.app.Presentation
import android.graphics.Color
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.MotionEvent
import android.view.Surface
import android.view.View
import android.view.WindowManager
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.compose.LocalActivityResultRegistryOwner
import androidx.activity.compose.LocalOnBackPressedDispatcherOwner
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.platform.ComposeView
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.lifecycle.setViewTreeViewModelStoreOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import app.mobilecad.freecad.AppState
import app.mobilecad.freecad.SelRef
import app.mobilecad.ui.OS
import app.mobilecad.ui.Shell
import app.mobilecad.ui.XrLayout
import kotlinx.coroutines.launch
import org.qtproject.qt.android.QtHostAccess
import java.io.File
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * MobileCAD XR: the whole MobileCAD app, in a headset. MobileCAD's interface (every workbench, feature dialogs, the
 * number pad, the feature tree) runs on a see-through virtual display in its XR layout, cut into surfaces around the
 * user (tree left, bars ahead, docked panels right) that you use with the controller ray as a touch screen. The model stands in the room and is MobileCAD's 3D view: the controller ray goes through that
 * view's own touch handling (picking, every sketch tool, Draft points, drag arrows), and what the view draws (the sketch
 * being edited, preselection, Draft feedback, annotations) is drawn on the model in 3D. Your hands move, turn, scale it.
 *
 * Without an OpenXR runtime (phone, emulator) the same scene is shown as a touch preview, so everything can be tested
 * without a headset.
 */
class XrActivity : ComponentActivity() {
    private lateinit var app: AppState
    private var panelTexture: SurfaceTexture? = null
    @Volatile private var panelFrame = false
    private var display: VirtualDisplay? = null
    private var presentation: Presentation? = null
    private var touchDown = 0L
    private var lastDoc = ""

    private external fun nativeAttach()
    private external fun nativeSetLayout(w: Int, h: Int, rects: FloatArray)
    private external fun nativeStartXr()
    private external fun nativeStop()
    private external fun nativeBeginScene(title: String)
    private external fun nativeAddMesh(name: String, tris: FloatArray, edges: FloatArray, r: Float, g: Float, b: Float, alpha: Float)
    private external fun nativeSetView(hoverObj: String, hoverSub: String, fade: Float, lines: FloatArray, dots: FloatArray, labelPos: FloatArray, labelText: Array<String>)
    private external fun nativeSetSketch(pixels: java.nio.ByteBuffer?, w: Int, h: Int, corners: FloatArray)
    private external fun nativeModelScale(): Float
    private external fun nativeSetPopups(rects: FloatArray)
    private external fun nativeEndScene(place: Boolean)
    private external fun nativeSetSelection(objs: Array<String>, subs: Array<String>)
    private external fun nativePreviewInit()
    private external fun nativePreviewTouch(action: Int, x: Float, y: Float)
    private external fun nativePreviewDraw(w: Int, h: Int)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        QtHostAccess.attach(this)                       // FreeCAD's QtCore needs a host activity (see MobileCAD)
        app = AppState(this)
        app.xr = true; app.xrChanged = { viewChanged() }
        app.start()
        app.whenReady { openFirstExample() }
        setContentView(TextView(this).apply {
            setBackgroundColor(Color.rgb(24, 27, 31)); setTextColor(Color.WHITE); textSize = 20f; gravity = android.view.Gravity.CENTER
            text = "MobileCAD XR\nPut on your headset"
        })
        nativeAttach()
        layoutPanel()
        nativeStartXr()
        syncScene()
    }

    override fun onStop() { super.onStop(); app.autosave() }
    override fun onDestroy() { nativeStop(); presentation?.dismiss(); display?.release(); super.onDestroy() }

    /** FreeCAD's example projects ship in the APK; the first one opens so there is something to look at. */
    private fun openFirstExample() {
        val dir = File(filesDir, "examples").apply { mkdirs() }
        for (n in assets.list("examples").orEmpty()) {
            val f = File(dir, n); if (!f.exists()) assets.open("examples/$n").use { i -> f.outputStream().use { i.copyTo(it) } }
        }
        if (app.objects.isEmpty()) File(dir, "PartDesignExample.FCStd").takeIf { it.exists() }?.let { app.open(it.absolutePath) }
    }

    // ---------------------------------------------------------------- the 3D model follows the document
    private fun syncScene() = lifecycleScope.launch {
        snapshotFlow { Triple(app.sceneVersion.intValue, app.docName, app.selection.toList()) }.collect { (_, doc, sel) ->
            val meshes = app.meshes
            nativeBeginScene(doc)
            for ((name, m) in meshes) {
                if (m.tris.isEmpty() && m.edges.isEmpty()) continue
                // as MobileCAD's view draws them: a failed feature's last good shape red and see-through, transparent objects
                val ghost = app.ghosts.containsKey(name)
                val c = if (ghost) 0xE0403F else app.colors[name] ?: 0xCFD6DC
                val alpha = if (ghost) 0.45f else if (name in app.transparent) 0.5f else 1f
                nativeAddMesh(name, m.tris, m.edges, ((c shr 16) and 255) / 255f, ((c shr 8) and 255) / 255f, (c and 255) / 255f, alpha)
            }
            nativeEndScene(doc != lastDoc)
            lastDoc = doc
            nativeSetSelection(sel.map { it.obj }.toTypedArray(), sel.map { it.sub }.toTypedArray())
            viewChanged()
        }
    }

    // ---------------------------------------------------------------- MobileCAD's 3D view, in the room
    /** Controller rays for the view (render thread): press / drag / release keep their order, consecutive drags and hovers
     *  collapse to the newest, so the UI thread never falls behind the headset's frame rate. */
    private val rays = ArrayDeque<FloatArray>()
    private var raysPosted = false
    @Suppress("unused") fun onXrViewport(action: Int, ox: Float, oy: Float, oz: Float, dx: Float, dy: Float, dz: Float, depth: Float) {
        val ev = floatArrayOf(action.toFloat(), ox, oy, oz, dx, dy, dz, depth)
        synchronized(rays) {
            val last = rays.lastOrNull()
            if (last != null && last[0] == ev[0] && (action == 1 || action == 3)) rays[rays.size - 1] = ev else rays.addLast(ev)
            if (raysPosted) return; raysPosted = true
        }
        runOnUiThread {
            val evs = synchronized(rays) { raysPosted = false; rays.toList().also { rays.clear() } }
            val v = app.viewport ?: return@runOnUiThread
            for (e in evs) {
                val a = e[0].toInt()
                // with a menu or dialog open, a press on the model is a touch outside it (it closes, as on a touch screen)
                if (a == 0) modal()?.let { w -> outsideTouch(w); swallow = true }
                if (swallow) { if (a == 2) swallow = false; if (a <= 2) continue }
                v.xrPointer(a, floatArrayOf(e[1], e[2], e[3]), floatArrayOf(e[4], e[5], e[6]), e[7])
            }
        }
    }
    private var swallow = false
    @Suppress("unused") fun onXrContextMenu() { runOnUiThread { app.viewport?.xrContextMenu() } }

    private var viewPosted = false
    private fun viewChanged() { if (viewPosted) return; viewPosted = true; window.decorView.postOnAnimation { viewPosted = false; sendView() } }

    private var skBmp: android.graphics.Bitmap? = null
    private var skBuf: java.nio.ByteBuffer? = null
    private var skRegion: DoubleArray? = null; private var skName = ""; private var skScale = 0f; private var skShown = false
    /** Sends what the view draws: its overlay as 3D primitives, and the sketch being edited drawn by its own editor onto a
     *  texture on the sketch plane, covering a stable region around the sketch at a resolution fit for its size in the room. */
    private fun sendView() {
        val v = app.viewport ?: return
        val o = v.xrOverlay()
        nativeSetView(o.hover?.first ?: "", o.hover?.second ?: "", if (o.dim) 0.35f else 1f, o.lines, o.dots,
            o.labels.flatMap { it.first.toList() }.toFloatArray(), o.labels.map { it.second }.toTypedArray())
        val sk = app.sketch; val b = v.xrSketchBounds()
        if (sk == null || b == null) { if (skShown) { nativeSetSketch(null, 0, 0, FloatArray(12)); skShown = false; skRegion = null }; return }
        val n = 1024; val scale = nativeModelScale().takeIf { it > 0f } ?: 0.001f        // m per mm
        var r = skRegion
        val inside = r != null && b[0] >= r[0] && b[1] >= r[1] && b[2] <= r[0] + r[2] && b[3] <= r[1] + r[2]
        if (r == null || !inside || sk.name != skName || kotlin.math.abs(scale / skScale - 1f) > 0.3f) {
            // at least half a metre across in the room, centred on the sketch
            val ext = maxOf(b[2] - b[0], b[3] - b[1]); val size = maxOf(ext * 1.4, 0.5 / scale)
            r = doubleArrayOf((b[0] + b[2]) / 2 - size / 2, (b[1] + b[3]) / 2 - size / 2, size); skRegion = r; skName = sk.name; skScale = scale
        }
        val bmp = skBmp ?: android.graphics.Bitmap.createBitmap(n, n, android.graphics.Bitmap.Config.ARGB_8888).also { skBmp = it }
        val metresPerPx = r[2] * scale / n
        val dpPx = maxOf(1f, (0.0012 / metresPerPx).toFloat())                        // a dp is ~1.2 mm in the room
        val corners = v.xrDrawSketch(bmp, r[0], r[1], r[2], dpPx) ?: return
        val buf = skBuf ?: java.nio.ByteBuffer.allocateDirect(n * n * 4).also { skBuf = it }
        buf.rewind(); bmp.copyPixelsToBuffer(buf)
        nativeSetSketch(buf, n, n, corners); skShown = true
    }

    // ---------------------------------------------------------------- callbacks from the XR core (render thread)
    @Suppress("unused") fun onXrResult(ok: Boolean, message: String) {
        Log.i("mcxr", "OpenXR: ${if (ok) "running on $message" else "unavailable ($message)"}")
        if (!ok) runOnUiThread { showPreview(message) }
    }
    @Suppress("unused") fun onXrExit() { runOnUiThread { finish() } }
    /** A trigger click on the model: the face / edge goes into the document's selection, as a tap in MobileCAD's view. */
    @Suppress("unused") fun onXrPick(obj: String, sub: String, x: Float, y: Float, z: Float) {
        runOnUiThread { if (obj.isEmpty()) app.clearSelection() else app.select(SelRef(obj, sub, floatArrayOf(x, y, z))); app.viewport?.sync() }
    }

    // ---------------------------------------------------------------- the UI panel: MobileCAD on a virtual display
    /** MobileCAD lays its interface out for XR (XrLayout): the virtual display's size and the parts the headset shows as
     *  separate surfaces around the user - feature list, view area, bars, docked panels (pixels at 240 dpi). */
    private fun layoutPanel() {
        val k = 1.5f; val t = XrLayout.TREE * k; val st = XrLayout.STAGE * k; val sd = XrLayout.SIDE * k
        val h = XrLayout.HEIGHT * k; val b = XrLayout.BARS * k
        nativeSetLayout((t + st + sd).toInt(), h.toInt(), floatArrayOf(0f, 0f, t, h,  t, b, st, h - b,  t, 0f, st, b,  t + st, 0f, sd, h))
    }

    /** Called on the GL thread with the panel's texture: MobileCAD's UI is rendered into it through a virtual display. */
    @Suppress("unused") fun createPanelSurface(tex: Int, w: Int, h: Int) {
        val st = SurfaceTexture(tex).apply { setDefaultBufferSize(w, h); setOnFrameAvailableListener { panelFrame = true } }
        panelTexture = st
        val surface = Surface(st)
        runOnUiThread {
            presentation?.dismiss(); display?.release()
            val dm = getSystemService(DisplayManager::class.java)
            val vd = dm.createVirtualDisplay("MobileCAD XR panel", w, h, 240, surface, DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY)
            display = vd
            presentation = Presentation(this, vd.display).apply {
                // see-through where MobileCAD draws nothing: the model in the room shows between the surfaces
                window?.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))
                window?.setFormat(android.graphics.PixelFormat.TRANSLUCENT)
                val view = ComposeView(context).apply {
                    setViewTreeLifecycleOwner(this@XrActivity); setViewTreeSavedStateRegistryOwner(this@XrActivity)
                    setViewTreeViewModelStoreOwner(this@XrActivity)
                    setContent {
                        val dark = app.theme == "dark" || (app.theme == "system" && isSystemInDarkTheme())
                        OS.dark = dark
                        val scheme = if (dark) darkColorScheme(primary = androidx.compose.ui.graphics.Color(0xFF8AB4F8),
                            surface = androidx.compose.ui.graphics.Color(0xFF1E2227), surfaceContainer = androidx.compose.ui.graphics.Color(0xFF242930),
                            background = androidx.compose.ui.graphics.Color(0xFF181B1F))
                            else lightColorScheme(primary = androidx.compose.ui.graphics.Color(0xFF2A6FC9))
                        // file pickers / back handling inside MobileCAD's shell resolve to this activity
                        CompositionLocalProvider(LocalActivityResultRegistryOwner provides this@XrActivity,
                                                 LocalOnBackPressedDispatcherOwner provides this@XrActivity) {
                            MaterialTheme(colorScheme = scheme) { Shell(app) }
                        }
                    }
                }
                window?.decorView?.let { d -> d.setViewTreeLifecycleOwner(this@XrActivity); d.setViewTreeSavedStateRegistryOwner(this@XrActivity); d.setViewTreeViewModelStoreOwner(this@XrActivity) }
                setContentView(view, ViewGroup.LayoutParams(w, h))
                show()
            }
        }
    }

    /** Called on the GL thread every frame: latch the newest UI image into the panel texture. */
    @Suppress("unused") fun updatePanelTexture(): Boolean {
        if (!panelFrame) return false
        panelFrame = false; try { panelTexture?.updateTexImage() } catch (e: Exception) { Log.w("mcxr", "panel", e) }
        if (!popupsPosted) { popupsPosted = true; runOnUiThread { popupsPosted = false; sendPopups() } }
        return true
    }

    /** The controller ray on the panel, as touches (trigger) / mouse hover and wheel (pointing, thumbstick) on the UI. */
    @Suppress("unused") fun onXrPanelPointer(action: Int, x: Float, y: Float) {
        runOnUiThread {
            if (presentation?.window?.decorView == null) return@runOnUiThread
            val now = SystemClock.uptimeMillis()
            fun at(w: View): Pair<Float, Float> { val o = IntArray(2); w.getLocationOnScreen(o); return Pair(x - o[0], y - o[1]) }
            fun touch(a: Int) {
                if (a == MotionEvent.ACTION_DOWN) { touchDown = now; touchWindow = windowFor(x, y, true) }
                val w = touchWindow ?: return
                val (lx, ly) = at(w)
                val ev = MotionEvent.obtain(touchDown, now, a, lx, ly, 0).apply { source = InputDevice.SOURCE_TOUCHSCREEN }
                w.dispatchTouchEvent(ev); ev.recycle()
                if (a == MotionEvent.ACTION_UP) touchWindow = null
            }
            fun mouse(a: Int, vscroll: Float = 0f) {
                val w = (if (a == MotionEvent.ACTION_HOVER_EXIT) hoverWindow else windowFor(x, y, false)) ?: return
                if (a == MotionEvent.ACTION_HOVER_MOVE && hoverWindow != null && hoverWindow != w) {
                    hoverWindow?.let { old -> val (ox, oy) = at(old); send(old, MotionEvent.ACTION_HOVER_EXIT, ox, oy, 0f, now) }
                }
                if (a != MotionEvent.ACTION_SCROLL) hoverWindow = if (a == MotionEvent.ACTION_HOVER_EXIT) null else w
                val (lx, ly) = at(w)
                send(w, a, lx, ly, vscroll, now)
            }
            when (action) {
                0 -> touch(MotionEvent.ACTION_DOWN)
                1 -> touch(MotionEvent.ACTION_MOVE)
                2 -> touch(MotionEvent.ACTION_UP)
                3 -> mouse(MotionEvent.ACTION_HOVER_MOVE)
                4 -> mouse(MotionEvent.ACTION_HOVER_EXIT)
                5 -> mouse(MotionEvent.ACTION_SCROLL, y)
            }
        }
    }

    @Volatile private var popupsPosted = false
    private var lastPopups = FloatArray(0)
    /** Where the menus, popups and dialogs over MobileCAD's window are: the headset shows each as a surface of its own. */
    private fun sendPopups() {
        val main = presentation?.window?.decorView ?: return
        val r = panelWindows().filter { it !== main }.flatMap { w -> val o = IntArray(2); w.getLocationOnScreen(o)
            listOf(o[0].toFloat(), o[1].toFloat(), w.width.toFloat(), w.height.toFloat()) }.toFloatArray()
        if (!r.contentEquals(lastPopups)) { lastPopups = r; nativeSetPopups(r) }
    }

    private var touchWindow: View? = null
    private var hoverWindow: View? = null

    private fun send(w: View, a: Int, x: Float, y: Float, vscroll: Float, now: Long) {
        val p = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_MOUSE }
        val c = MotionEvent.PointerCoords().apply { this.x = x; this.y = y; if (vscroll != 0f) setAxisValue(MotionEvent.AXIS_VSCROLL, vscroll) }
        val ev = MotionEvent.obtain(now, now, a, 1, arrayOf(p), arrayOf(c), 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_MOUSE, 0)
        w.dispatchGenericMotionEvent(ev); ev.recycle()
    }

    /** The windows on the panel's display, bottom to top: the Presentation plus the menus, popups and dialogs opened from
     *  it, which are windows of their own (a dispatch to the Presentation never reaches them). Same lookup as Espresso. */
    private fun panelWindows(): List<View> {
        val main = presentation?.window?.decorView ?: return emptyList()
        val id = main.display?.displayId ?: return listOf(main)
        return try {
            val c = Class.forName("android.view.WindowManagerGlobal")
            val f = c.getDeclaredField("mViews").apply { isAccessible = true }
            @Suppress("UNCHECKED_CAST")
            val all = (f.get(c.getMethod("getInstance").invoke(null)) as List<View>).toList()
            all.filter { it.isAttachedToWindow && it.visibility == View.VISIBLE && it.display?.displayId == id }.ifEmpty { listOf(main) }
        } catch (e: Exception) { Log.w("mcxr", "window list", e); listOf(main) }
    }

    /** The window a pointer at (x, y) goes to: the topmost one under it, unless a focusable (modal) window lies above -
     *  then that one, which closes itself on a touch outside, as a menu or dialog does on a touch screen. */
    private fun windowFor(x: Float, y: Float, touch: Boolean): View? {
        val ws = panelWindows()
        for (w in ws.asReversed()) {
            val o = IntArray(2); w.getLocationOnScreen(o)
            if (x >= o[0] && y >= o[1] && x < o[0] + w.width && y < o[1] + w.height) return w
            val lp = w.layoutParams as? WindowManager.LayoutParams
            if (touch && w !== presentation?.window?.decorView && lp != null && lp.flags and WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE == 0) return w
        }
        return ws.firstOrNull()
    }

    /** The open menu / popup / dialog (a focusable window above MobileCAD's), if any. */
    private fun modal(): View? = panelWindows().lastOrNull { w -> w !== presentation?.window?.decorView &&
        (w.layoutParams as? WindowManager.LayoutParams)?.let { it.flags and WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE == 0 } == true }
    private fun outsideTouch(w: View) {
        val now = SystemClock.uptimeMillis()
        for (a in intArrayOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val ev = MotionEvent.obtain(now, now, a, -10000f, -10000f, 0).apply { source = InputDevice.SOURCE_TOUCHSCREEN }
            w.dispatchTouchEvent(ev); ev.recycle()
        }
    }

    // ---------------------------------------------------------------- no headset: touch preview of the XR scene
    private fun showPreview(reason: String) {
        val view = GLSurfaceView(this)
        view.setEGLContextClientVersion(3)
        view.setRenderer(object : GLSurfaceView.Renderer {
            var w = 1; var h = 1
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) { nativePreviewInit() }
            override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) { w = width; h = height }
            override fun onDrawFrame(gl: GL10?) { nativePreviewDraw(w, h) }
        })
        // one finger: the controller with its trigger pressed; two fingers: look around
        view.setOnTouchListener { _, e ->
            val cx = (0 until e.pointerCount).map { e.getX(it) }.average().toFloat(); val cy = (0 until e.pointerCount).map { e.getY(it) }.average().toFloat()
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> nativePreviewTouch(0, e.x, e.y)
                MotionEvent.ACTION_POINTER_DOWN -> nativePreviewTouch(4, cx, cy)
                MotionEvent.ACTION_MOVE -> if (e.pointerCount >= 2) nativePreviewTouch(3, cx, cy) else nativePreviewTouch(1, e.x, e.y)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> nativePreviewTouch(2, e.x, e.y)
            }
            true
        }
        val note = TextView(this).apply {
            setTextColor(Color.rgb(200, 208, 216)); textSize = 13f; setPadding(28, 18, 28, 18); setBackgroundColor(Color.argb(140, 0, 0, 0))
            text = "No headset ($reason): preview of the XR scene. One finger is the controller (touch the UI to use it, the model or the space around it to pick, sketch and place points); two fingers look around."
        }
        setContentView(FrameLayout(this).apply {
            addView(view)
            addView(note, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, android.view.Gravity.TOP))
        })
    }

    companion object { init { System.loadLibrary("mcxr") } }
}
