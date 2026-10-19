package app.mobilecad.xr

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.util.Log
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.widget.Button
import android.widget.FrameLayout
import android.widget.TextView
import app.mobilecad.freecad.Engine
import app.mobilecad.freecad.FcObject
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.qtproject.qt.android.QtHostAccess
import java.io.File
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * MobileCAD XR: FreeCAD documents in OpenXR. On a headset (Meta Quest 3, other Android OpenXR devices) the model floats in
 * front of you: aim + trigger selects a face, holding trigger/grip moves it, both hands scale and turn it, A/X opens the
 * next document. Without an OpenXR runtime (phone, emulator) the same scene is shown as a touch preview.
 * The CAD engine is MobileCAD's (mobilecad/ submodule): documents are opened and tessellated exactly as in the phone app.
 */
class XrActivity : Activity() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val engine by lazy { Engine.get(this) }
    private lateinit var status: TextView
    private var docs: List<File> = emptyList()
    private var current = 0
    private var title = "MobileCAD XR"; private var subtitle = "Starting the CAD engine…"; private var selection = ""
    @Volatile private var engineReady = false

    private external fun nativeAttach()
    private external fun nativeStartXr()
    private external fun nativeStop()
    private external fun nativeBeginScene(title: String)
    private external fun nativeAddMesh(name: String, tris: FloatArray, edges: FloatArray, r: Float, g: Float, b: Float)
    private external fun nativeEndScene()
    private external fun nativeSetPanel(argb: IntArray, w: Int, h: Int)
    private external fun nativePreviewInit()
    private external fun nativePreviewDraw(w: Int, h: Int, dx: Float, dy: Float, zoom: Float, tx: Float, ty: Float, touching: Boolean, menu: Boolean)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        QtHostAccess.attach(this)                       // FreeCAD's QtCore needs a host activity (see MobileCAD)
        status = TextView(this).apply { setTextColor(Color.WHITE); textSize = 18f; gravity = Gravity.CENTER; text = subtitle }
        setContentView(FrameLayout(this).apply { setBackgroundColor(Color.rgb(28, 33, 41)); addView(status) })
        nativeAttach()
        nativeStartXr()                                  // the headset view starts at once; the model follows when loaded
        docs = copyExamples()
        updatePanel()
        scope.launch {
            try {
                engine.start { p -> runOnUiThread { subtitle = p; status.text = p } }
                engineReady = true
                load(0)
            } catch (e: Throwable) { Log.e("mcxr", "engine", e); subtitle = "The CAD engine failed to start"; status.text = e.message; updatePanel() }
        }
    }

    override fun onDestroy() { nativeStop(); scope.cancel(); super.onDestroy() }

    // ---------------------------------------------------------------- documents
    /** FreeCAD's example projects ship in the APK (assets/examples); they are copied out once so FreeCAD can open them. */
    private fun copyExamples(): List<File> {
        val dir = File(filesDir, "examples").apply { mkdirs() }
        for (n in assets.list("examples").orEmpty()) {
            val f = File(dir, n); if (!f.exists()) assets.open("examples/$n").use { i -> f.outputStream().use { i.copyTo(it) } }
        }
        return dir.listFiles { f -> f.name.endsWith(".FCStd", true) }.orEmpty().sortedBy { it.name }
    }

    private fun load(index: Int) {
        if (docs.isEmpty() || !engineReady) return
        current = (index % docs.size + docs.size) % docs.size
        val file = docs[current]
        subtitle = "Opening ${file.name}…"; selection = ""; updatePanel()
        scope.launch {
            val meshes = withContext(engine.dispatcher) {
                engine.run("import FreeCAD as App\nfor n in list(App.listDocuments()): App.closeDocument(n)")
                engine.openDocument(file.absolutePath)
                val colors = (engine.call("view_data") as? JSONObject)?.optJSONObject("colors")
                shown(engine.objects()).mapNotNull { o ->
                    val m = engine.tessellate(o.name)
                    if (m.tris.isEmpty() && m.edges.isEmpty()) null else {
                        val c = colors?.optJSONArray(o.name)
                        Triple(o.label, m, if (c != null) floatArrayOf(c.getDouble(0).toFloat(), c.getDouble(1).toFloat(), c.getDouble(2).toFloat())
                                           else floatArrayOf(0.81f, 0.84f, 0.86f))
                    }
                }
            }
            title = file.nameWithoutExtension
            nativeBeginScene(title)
            for ((label, m, c) in meshes) nativeAddMesh(label, m.tris, m.edges, c[0], c[1], c[2])
            nativeEndScene()
            subtitle = "${meshes.size} objects · file ${current + 1} of ${docs.size}"; status.text = "$title\n$subtitle"; updatePanel()
        }
    }

    /** What MobileCAD shows in its Part / Assembly tabs: the features themselves, not containers or origin geometry. */
    private fun shown(objs: List<FcObject>): List<FcObject> {
        val byName = objs.associateBy { it.name }
        if (objs.any { it.type == "Assembly::AssemblyObject" }) return objs.filter { it.assembly != null && it.type == "App::Link" && it.visible }
        val hidden = setOf("PartDesign::Body", "App::Part", "App::Origin", "App::DocumentObjectGroup", "App::Line", "App::Plane", "App::Point")
        return objs.filter { o ->
            o.hasShape && o.visible && o.assembly == null && o.type !in hidden &&
                o.parents.none { byName[it]?.type == "App::Origin" } &&
                objs.none { p -> p.type == "PartDesign::Body" && o.name in p.group && !p.visible }
        }
    }

    // ---------------------------------------------------------------- callbacks from the XR core (render thread)
    @Suppress("unused") fun onXrResult(ok: Boolean, message: String) {
        Log.i("mcxr", "OpenXR: ${if (ok) "running on $message" else "unavailable ($message)"}")
        if (!ok) runOnUiThread { showPreview(message) }
    }
    @Suppress("unused") fun onXrSelect(obj: String, face: Int) { runOnUiThread { selection = if (face > 0) "$obj · Face$face" else ""; updatePanel() } }
    @Suppress("unused") fun onXrNextDocument() { runOnUiThread { load(current + 1) } }
    @Suppress("unused") fun onXrExit() { runOnUiThread { finish() } }

    /** The info panel above the left controller (and the preview's overlay): document, selection, controls. */
    private fun updatePanel() {
        val w = 640; val h = 300
        val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888); val c = Canvas(bmp)
        val bg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(235, 24, 28, 34) }
        c.drawRoundRect(RectF(0f, 0f, w.toFloat(), h.toFloat()), 28f, 28f, bg)
        val big = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textSize = 46f; typeface = Typeface.DEFAULT_BOLD }
        val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(200, 208, 216); textSize = 30f }
        val acc = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(108, 168, 255); textSize = 30f; typeface = Typeface.DEFAULT_BOLD }
        c.drawText(title.take(24), 32f, 70f, big)
        c.drawText(subtitle.take(40), 32f, 118f, txt)
        c.drawText(if (selection.isEmpty()) "Aim and click a face to select it" else "Selected: ${selection.take(30)}", 32f, 172f, if (selection.isEmpty()) txt else acc)
        c.drawText("Hold trigger: move · both hands: scale", 32f, 226f, txt)
        c.drawText("A / X: next file", 32f, 270f, txt)
        val px = IntArray(w * h); bmp.getPixels(px, 0, w, 0, 0, w, h); bmp.recycle()
        nativeSetPanel(px, w, h)
    }

    // ---------------------------------------------------------------- no headset: touch preview
    private fun showPreview(reason: String) {
        var dx = 0f; var dy = 0f; var zoom = 1f; var tapX = 0f; var tapY = 0f; var tapFrames = 0; var menu = false
        val view = GLSurfaceView(this)
        view.setEGLContextClientVersion(3)
        view.setRenderer(object : GLSurfaceView.Renderer {
            var w = 1; var h = 1
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) { nativePreviewInit(); updatePanel() }
            override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) { w = width; h = height }
            override fun onDrawFrame(gl: GL10?) {
                val (ddx, ddy, z) = synchronized(this@XrActivity) { Triple(dx, dy, zoom).also { dx = 0f; dy = 0f; zoom = 1f } }
                // a tap is a short trigger click: pressed for one frame, released the next
                val touching = tapFrames == 2; if (tapFrames > 0) tapFrames--
                val m = menu; menu = false
                nativePreviewDraw(w, h, ddx, ddy, z, tapX, tapY, touching, m)
            }
        })
        val gestures = GestureDetector(this, object : GestureDetector.SimpleOnGestureListener() {
            override fun onScroll(e1: MotionEvent?, e2: MotionEvent, distanceX: Float, distanceY: Float): Boolean {
                synchronized(this@XrActivity) { dx += distanceX * 0.006f; dy += distanceY * 0.006f }; return true
            }
            override fun onSingleTapUp(e: MotionEvent): Boolean {
                tapX = e.x / view.width * 2 - 1; tapY = 1 - e.y / view.height * 2; tapFrames = 2; return true
            }
        })
        val scale = ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean { synchronized(this@XrActivity) { zoom /= d.scaleFactor }; return true }
        })
        view.setOnTouchListener { _, e -> scale.onTouchEvent(e); if (!scale.isInProgress) gestures.onTouchEvent(e); true }
        val note = TextView(this).apply {
            setTextColor(Color.rgb(200, 208, 216)); textSize = 14f; setPadding(32, 24, 32, 24)
            text = "No headset ($reason) – preview. Drag to look around, pinch to zoom, tap a face to select it."
        }
        val next = Button(this).apply { text = "Next file"; setOnClickListener { menu = true } }
        setContentView(FrameLayout(this).apply {
            addView(view)
            addView(note, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP))
            addView(next, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.END).apply { setMargins(32, 32, 32, 32) })
        })
    }

    companion object { init { System.loadLibrary("mcxr") } }
}
