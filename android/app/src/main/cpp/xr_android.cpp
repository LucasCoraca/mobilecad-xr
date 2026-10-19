// Android glue for the XR core. The headset path runs OpenXR on its own render thread (EGL pbuffer context); without an
// OpenXR runtime (phone, emulator) the same scene is drawn into a GLSurfaceView as a touch preview. In both, the app's UI
// (MobileCAD's Compose shell on a virtual display) arrives as a SurfaceTexture shown on the floating panel; the Kotlin side
// feeds the model (the document's meshes + selection) and receives picks and panel touches.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/log.h>
#include <jni.h>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "gl.h"
#include "interaction.h"
#include "renderer.h"
#include "xr_session.h"
#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65   // GL_OES_EGL_image_external (SurfaceTexture images)
#endif

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mcxr", __VA_ARGS__)
using namespace mcxr;

namespace {
JavaVM* gVm = nullptr; jobject gActivity = nullptr;
Interaction gIa; Renderer gRenderer;
std::mutex gQueueLock; std::vector<std::function<void()>> gQueue;   // scene / selection changes, applied on the GL thread
std::thread gXrThread; std::atomic<bool> gStop{false};
std::atomic<bool> gPlaceModel{true}, gPlacePanel{true};
constexpr int kPanelW = 1600, kPanelH = 1000;                       // the UI's virtual display, in pixels

JNIEnv* env() { JNIEnv* e = nullptr; gVm->GetEnv((void**)&e, JNI_VERSION_1_6); return e; }
jmethodID method(JNIEnv* e, const char* name, const char* sig) {
    jclass c = e->GetObjectClass(gActivity); jmethodID m = e->GetMethodID(c, name, sig); e->DeleteLocalRef(c); return m;
}
void post(std::function<void()> f) { std::lock_guard<std::mutex> l(gQueueLock); gQueue.push_back(std::move(f)); }
void drainQueue() {
    std::vector<std::function<void()>> q; { std::lock_guard<std::mutex> l(gQueueLock); q.swap(gQueue); }
    for (auto& f : q) f();
}

/** On the GL thread: the UI panel's texture (Java wraps it in a SurfaceTexture and renders MobileCAD's UI into it). */
void createPanel() {
    GLuint tex = 0; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gIa.panel.tex = tex; gIa.panel.external = true; gIa.panel.pxW = kPanelW; gIa.panel.pxH = kPanelH;
    gIa.panel.width = 1.0f; gIa.panel.height = 1.0f * kPanelH / kPanelW; gIa.panel.visible = true;
    JNIEnv* e = env(); e->CallVoidMethod(gActivity, method(e, "createPanelSurface", "(III)V"), (jint)tex, kPanelW, kPanelH);
}
void updatePanel() { JNIEnv* e = env(); e->CallVoidMethod(gActivity, method(e, "updatePanelTexture", "()V")); }

void hookCallbacks() {
    gIa.hostSelection = true;
    gIa.onPick = [](const ElemRef& r, V3 p) {
        JNIEnv* e = env();
        std::string obj = r.valid() ? gIa.scene.meshes[r.mesh].name : "", sub = r.valid() ? std::string(r.kind == 'E' ? "Edge" : "Face") + std::to_string(r.id) : "";
        jstring jo = e->NewStringUTF(obj.c_str()), js = e->NewStringUTF(sub.c_str());
        e->CallVoidMethod(gActivity, method(e, "onXrPick", "(Ljava/lang/String;Ljava/lang/String;FFF)V"), jo, js, p.x, p.y, p.z);
        e->DeleteLocalRef(jo); e->DeleteLocalRef(js);
    };
    gIa.onPanelPointer = [](int action, float x, float y) {
        JNIEnv* e = env(); e->CallVoidMethod(gActivity, method(e, "onXrPanelPointer", "(IFF)V"), action, x, y);
    };
    gIa.onMenu = [] { gPlaceModel = true; gPlacePanel = true; };     // menu / A / X: bring the panel and the model back in front
}

/** The headset loop: EGL pbuffer context (OpenXR renders into its own swapchains), session, frames until told to stop. */
void xrMain() {
    JNIEnv* e = nullptr; gVm->AttachCurrentThread(&e, nullptr);
    auto report = [&](bool ok, const std::string& msg) {
        jstring s = e->NewStringUTF(msg.c_str()); e->CallVoidMethod(gActivity, method(e, "onXrResult", "(ZLjava/lang/String;)V"), (jboolean)ok, s); e->DeleteLocalRef(s);
    };
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(dpy, nullptr, nullptr);
    const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                               EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE};
    EGLConfig cfg = nullptr; EGLint n = 0; eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n);
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = n ? eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs) : EGL_NO_CONTEXT;
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surf = ctx ? eglCreatePbufferSurface(dpy, cfg, pb) : EGL_NO_SURFACE;
    if (!ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) { report(false, "No OpenGL ES 3 context"); gVm->DetachCurrentThread(); return; }
    Headset hs; PlatformInit p; p.vm = gVm; p.activity = gActivity; p.display = dpy; p.config = cfg; p.context = ctx;
    std::string err;
    if (!hs.create(p, "MobileCAD XR", &err)) {
        report(false, err);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx);
        gVm->DetachCurrentThread(); return;
    }
    gRenderer.contextLost(); gRenderer.init(); hookCallbacks(); createPanel();
    hs.onPlaced = [](const Pose& head) { if (gPlacePanel.exchange(false)) gIa.placePanel(head); };
    report(true, hs.systemName);
    bool quit = false;
    while (!gStop && !quit) {
        hs.pollEvents(&quit);
        drainQueue();
        if (gPlaceModel.exchange(false)) hs.needsPlacement = true;
        if (gPlacePanel) hs.needsPlacement = true;                     // onPlaced puts the panel in front of the head
        if (!hs.running()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        updatePanel();
        hs.frame(gIa, gRenderer);
    }
    hs.destroy();
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx);
    if (quit) e->CallVoidMethod(gActivity, method(e, "onXrExit", "()V"));
    gVm->DetachCurrentThread();
}

// ---- headset-free preview: you stand at the origin; drag to look around, the touch is your controller
struct Preview {
    float yaw = 0, pitch = -0.12f, fov = 0.62f;
    int mode = 0;                 // 0 idle, 1 touching the panel (trigger held), 2 looking around
    float downX = 0, downY = 0, lastX = 0, lastY = 0, tx = 0, ty = 0; int clickFrames = 0; bool moved = false;
    bool pressSeen = false, releasePending = false;   // a tap on the panel is held for at least one frame
    int w = 1, h = 1;
} gPv;
const Pose kHead{V3{0, 1.6f, 0}, Q{}, 1};
Pose previewEye() { return {kHead.pos, qnorm(axisAngle({0, 1, 0}, gPv.yaw) * axisAngle({1, 0, 0}, gPv.pitch)), 1}; }
Pose previewRay(float sx, float sy) {               // screen point (pixels) -> aim pose from the eye
    Pose eye = previewEye(); float aspect = (float)gPv.w / std::max(gPv.h, 1), fy = gPv.fov, fx = std::atan(std::tan(fy) * aspect);
    float nx = sx / gPv.w * 2 - 1, ny = 1 - sy / gPv.h * 2;
    V3 d = norm(rotate(eye.rot, V3{nx * std::tan(fx), ny * std::tan(fy), -1}));
    V3 z = d * -1.f, xa = norm(cross(V3{0, 1, 0}, z)), ya = cross(z, xa); float tr = xa.x + ya.y + z.z; Q q{};
    if (tr > 0) { float s = std::sqrt(tr + 1) * 2; q = {(ya.z - z.y) / s, (z.x - xa.z) / s, (xa.y - ya.x) / s, s / 4}; }
    return {eye.pos, qnorm(q), 1};
}
}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) { gVm = vm; return JNI_VERSION_1_6; }

JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeAttach(JNIEnv* e, jobject self) {
    if (gActivity) e->DeleteGlobalRef(gActivity);
    gActivity = e->NewGlobalRef(self);
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeStartXr(JNIEnv*, jobject) {
    if (gXrThread.joinable()) return;
    gStop = false; gXrThread = std::thread(xrMain);
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeStop(JNIEnv*, jobject) {
    gStop = true; if (gXrThread.joinable()) gXrThread.join();
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeBeginScene(JNIEnv* e, jobject, jstring title) {
    const char* t = e->GetStringUTFChars(title, nullptr); std::string s(t); e->ReleaseStringUTFChars(title, t);
    post([s] { Pose keep = gIa.model; gIa.scene.clear(); gIa.scene.title = s; gIa.model = keep; });
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeAddMesh(JNIEnv* e, jobject, jstring name, jfloatArray tris, jfloatArray edges,
                                                                      jfloat r, jfloat g, jfloat b) {
    const char* n = e->GetStringUTFChars(name, nullptr); std::string sname(n); e->ReleaseStringUTFChars(name, n);
    jsize nt = e->GetArrayLength(tris), ne = e->GetArrayLength(edges);              // engine layout: [n][tris][n][edges]
    std::vector<uint8_t> blob(8 + 4 * (nt + ne)); int32_t a = nt, c = ne;
    memcpy(blob.data(), &a, 4); e->GetFloatArrayRegion(tris, 0, nt, (jfloat*)(blob.data() + 4));
    memcpy(blob.data() + 4 + 4 * nt, &c, 4); e->GetFloatArrayRegion(edges, 0, ne, (jfloat*)(blob.data() + 8 + 4 * nt));
    post([sname, blob = std::move(blob), r, g, b] { float rgb[3] = {r, g, b}; gIa.scene.addTessellation(sname, blob.data(), blob.size(), rgb); });
}
/** `place`: a new document - put the model in front of the viewer (an edit of the same document keeps where it is). */
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeEndScene(JNIEnv*, jobject, jboolean place) {
    if (place) post([] { gPlaceModel = true; });
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeSetSelection(JNIEnv* e, jobject, jobjectArray objs, jobjectArray subs) {
    std::vector<std::pair<std::string, std::string>> sel;
    for (jsize i = 0, n = e->GetArrayLength(objs); i < n; ++i) {
        auto o = (jstring)e->GetObjectArrayElement(objs, i), s = (jstring)e->GetObjectArrayElement(subs, i);
        const char* co = e->GetStringUTFChars(o, nullptr); const char* cs = e->GetStringUTFChars(s, nullptr);
        sel.emplace_back(co, cs); e->ReleaseStringUTFChars(o, co); e->ReleaseStringUTFChars(s, cs);
        e->DeleteLocalRef(o); e->DeleteLocalRef(s);
    }
    post([sel] {
        gIa.selected.clear();
        for (auto& [o, s] : sel) {
            int m = gIa.scene.find(o); if (m < 0) continue;
            if (s.rfind("Face", 0) == 0) gIa.selected.push_back({m, 'F', std::atoi(s.c_str() + 4)});
            else if (s.rfind("Edge", 0) == 0) gIa.selected.push_back({m, 'E', std::atoi(s.c_str() + 4)});
        }
    });
}

// ---- preview (GLSurfaceView thread) ----
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativePreviewInit(JNIEnv*, jobject) {
    gRenderer.contextLost(); gRenderer.init(); hookCallbacks(); createPanel(); gPlaceModel = true; gPlacePanel = true;
}
/** Touch on the preview: 0 down, 1 move, 2 up (pixels). On the panel it is a touch; elsewhere a drag looks around and a tap picks. */
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativePreviewTouch(JNIEnv*, jobject, jint action, jfloat x, jfloat y) {
    post([action, x, y] {
        if (action == 0) {
            float t, px, py; gPv.downX = gPv.lastX = gPv.tx = x; gPv.downY = gPv.lastY = gPv.ty = y; gPv.moved = false;
            Pose aim = previewRay(x, y);
            V3 n = rotate(gIa.panel.pose.rot, {0, 0, 1}), d = rotate(aim.rot, {0, 0, -1}); float den = dot(d, n);
            bool onPanel = false;
            if (gIa.panel.visible && std::fabs(den) > 1e-5f) {
                t = dot(gIa.panel.pose.pos - aim.pos, n) / den;
                V3 l = rotate(conj(gIa.panel.pose.rot), aim.pos + d * t - gIa.panel.pose.pos);
                px = l.x / gIa.panel.width + 0.5f; py = 0.5f - l.y / gIa.panel.height;
                onPanel = t > 0 && px >= 0 && px <= 1 && py >= 0 && py <= 1;
            }
            gPv.mode = onPanel ? 1 : 2; gPv.pressSeen = false; gPv.releasePending = false;
        } else if (action == 1) {
            if (gPv.mode == 2) { gPv.yaw += (x - gPv.lastX) * 0.004f; gPv.pitch = std::max(-1.2f, std::min(1.2f, gPv.pitch + (y - gPv.lastY) * 0.004f)); }
            else gPv.tx = x, gPv.ty = y;
            if (std::fabs(x - gPv.downX) + std::fabs(y - gPv.downY) > 12) gPv.moved = true;
            gPv.lastX = x; gPv.lastY = y;
        } else {
            if (gPv.mode == 2 && !gPv.moved) { gPv.tx = x; gPv.ty = y; gPv.clickFrames = 2; }     // tap = trigger click
            if (gPv.mode == 1 && !gPv.pressSeen) gPv.releasePending = true;                        // release after one pressed frame
            gPv.mode = 0;
        }
    });
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativePreviewDraw(JNIEnv*, jobject, jint w, jint h) {
    gPv.w = w; gPv.h = h;
    drainQueue();
    if (gPlacePanel.exchange(false)) gIa.placePanel(kHead);
    if (!gIa.scene.empty() && gPlaceModel.exchange(false)) gIa.placeModel(kHead);
    updatePanel();
    HandInput hands[2];
    hands[1].active = true; hands[1].aim = previewRay(gPv.tx, gPv.ty); hands[1].grip = hands[1].aim;
    bool panelPress = gPv.mode == 1 || gPv.releasePending;
    if (panelPress && gPv.pressSeen) gPv.releasePending = false, panelPress = gPv.mode == 1;
    if (panelPress) gPv.pressSeen = true;
    hands[1].select = (panelPress || gPv.clickFrames == 2) ? 1.f : 0.f;
    if (gPv.clickFrames > 0) gPv.clickFrames--;
    gIa.update(hands, 1.f / 60.f);
    Pose eye = previewEye(); float aspect = h > 0 ? (float)w / h : 1.f, fy = gPv.fov, fx = std::atan(std::tan(fy) * aspect);
    M4 vp = mul(projectionFov(-fx, fx, fy, -fy, 0.02f, 50.f), viewFrom(eye));
    gRenderer.gamma = 1.f; gRenderer.beginView(0, 0, 0, w, h, true);
    gIa.render(gRenderer, vp, eye.pos);
}

}  // extern "C"
