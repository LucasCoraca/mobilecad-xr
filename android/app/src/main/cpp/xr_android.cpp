// Android glue for the XR core: an OpenXR render thread with its own EGL context (headset), or - when the device has no
// OpenXR runtime (phone, emulator) - a flat preview drawn into a GLSurfaceView, driven by touch. The Kotlin activity feeds
// the scene (meshes from MobileCAD's engine) and the info panel; picks and button presses come back as callbacks.
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
#include "interaction.h"
#include "renderer.h"
#include "xr_session.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mcxr", __VA_ARGS__)
using namespace mcxr;

namespace {
JavaVM* gVm = nullptr; jobject gActivity = nullptr;
Interaction gIa; Renderer gRenderer; bool gRendererReady = false;
std::mutex gQueueLock; std::vector<std::function<void()>> gQueue;   // scene / panel changes, applied on the GL thread
std::thread gXrThread; std::atomic<bool> gStop{false};
bool gPlace = true;                                                  // place the model in front of the viewer

/** Runs `f` with a JNIEnv for the current thread (attaching it if needed). */
void withEnv(const std::function<void(JNIEnv*)>& f) {
    JNIEnv* env = nullptr; bool attached = false;
    if (gVm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) { gVm->AttachCurrentThread(&env, nullptr); attached = true; }
    f(env);
    if (attached) gVm->DetachCurrentThread();
}
void callActivity(const char* method, const char* sig, ...) {
    va_list args; va_start(args, sig);
    JNIEnv* env = nullptr;
    if (gVm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && gActivity) {
        jclass c = env->GetObjectClass(gActivity); jmethodID m = env->GetMethodID(c, method, sig);
        if (m) env->CallVoidMethodV(gActivity, m, args);
        env->DeleteLocalRef(c);
    }
    va_end(args);
}
void hookCallbacks() {
    gIa.onSelect = [](const std::string& obj, int face) {
        JNIEnv* env = nullptr; if (gVm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return;
        jstring s = env->NewStringUTF(obj.c_str()); callActivity("onXrSelect", "(Ljava/lang/String;I)V", s, face); env->DeleteLocalRef(s);
    };
    gIa.onNextDocument = [] { callActivity("onXrNextDocument", "()V"); };
}
void drainQueue() {
    std::vector<std::function<void()>> q; { std::lock_guard<std::mutex> l(gQueueLock); q.swap(gQueue); }
    for (auto& f : q) f();
}
void post(std::function<void()> f) { std::lock_guard<std::mutex> l(gQueueLock); gQueue.push_back(std::move(f)); }

/** The headset loop: EGL pbuffer context (OpenXR renders into its own swapchains), session, frames until told to stop. */
void xrMain() {
    withEnv([](JNIEnv* env) {
        auto report = [&](bool ok, const std::string& msg) {
            jclass c = env->GetObjectClass(gActivity); jmethodID m = env->GetMethodID(c, "onXrResult", "(ZLjava/lang/String;)V");
            jstring s = env->NewStringUTF(msg.c_str()); env->CallVoidMethod(gActivity, m, (jboolean)ok, s); env->DeleteLocalRef(s); env->DeleteLocalRef(c);
        };
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(dpy, nullptr, nullptr);
        const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                   EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_NONE};
        EGLConfig cfg = nullptr; EGLint n = 0; eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n);
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLContext ctx = n ? eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs) : EGL_NO_CONTEXT;
        const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        EGLSurface surf = ctx ? eglCreatePbufferSurface(dpy, cfg, pb) : EGL_NO_SURFACE;
        if (!ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) { report(false, "No OpenGL ES 3 context"); return; }
        gRenderer.init(); gRendererReady = true; hookCallbacks();
        Headset hs; PlatformInit p; p.vm = gVm; p.activity = gActivity; p.display = dpy; p.config = cfg; p.context = ctx;
        std::string err;
        if (!hs.create(p, "MobileCAD XR", &err)) {
            report(false, err);
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx);
            gRendererReady = false; return;
        }
        report(true, hs.systemName);
        bool quit = false;
        while (!gStop && !quit) {
            hs.pollEvents(&quit);
            drainQueue();
            if (gPlace) { hs.needsPlacement = true; gPlace = false; }
            if (!hs.running()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            hs.frame(gIa, gRenderer);
        }
        hs.destroy();
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx);
        if (quit) callActivity("onXrExit", "()V");
    });
}

// flat preview (no headset): a camera orbiting the model, a "hand" ray from the camera through the touched point
struct Preview { float yaw = 0, pitch = -0.15f, dist = 0.9f; } gPv;
}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) { gVm = vm; return JNI_VERSION_1_6; }

JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeAttach(JNIEnv* env, jobject self) {
    if (gActivity) env->DeleteGlobalRef(gActivity);
    gActivity = env->NewGlobalRef(self);
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeStartXr(JNIEnv*, jobject) {
    if (gXrThread.joinable()) return;
    gStop = false; gXrThread = std::thread(xrMain);
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeStop(JNIEnv*, jobject) {
    gStop = true; if (gXrThread.joinable()) gXrThread.join();
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeBeginScene(JNIEnv* env, jobject, jstring title) {
    const char* t = env->GetStringUTFChars(title, nullptr); std::string s(t); env->ReleaseStringUTFChars(title, t);
    post([s] { gIa.scene.clear(); gIa.scene.title = s; gIa.selected = {}; });
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeAddMesh(JNIEnv* env, jobject, jstring name, jfloatArray tris, jfloatArray edges,
                                                                      jfloat r, jfloat g, jfloat b) {
    const char* n = env->GetStringUTFChars(name, nullptr); std::string sname(n); env->ReleaseStringUTFChars(name, n);
    // repack into the engine's tessellation layout: [int32 n][tris][int32 n][edges]
    jsize nt = env->GetArrayLength(tris), ne = env->GetArrayLength(edges);
    std::vector<uint8_t> blob(8 + 4 * (nt + ne));
    int32_t a = nt, c = ne; memcpy(blob.data(), &a, 4);
    env->GetFloatArrayRegion(tris, 0, nt, (jfloat*)(blob.data() + 4));
    memcpy(blob.data() + 4 + 4 * nt, &c, 4);
    env->GetFloatArrayRegion(edges, 0, ne, (jfloat*)(blob.data() + 8 + 4 * nt));
    float rgb[3] = {r, g, b};
    post([sname, blob = std::move(blob), rgb0 = rgb[0], rgb1 = rgb[1], rgb2 = rgb[2]] {
        float c3[3] = {rgb0, rgb1, rgb2}; gIa.scene.addTessellation(sname, blob.data(), blob.size(), c3); });
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeEndScene(JNIEnv*, jobject) {
    post([] { gPlace = true; gPv = Preview{}; });
}
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativeSetPanel(JNIEnv* env, jobject, jintArray argb, jint w, jint h) {
    std::vector<uint8_t> px((size_t)w * h * 4); std::vector<jint> src((size_t)w * h);
    env->GetIntArrayRegion(argb, 0, w * h, src.data());
    for (size_t i = 0; i < src.size(); ++i) { uint32_t v = (uint32_t)src[i]; px[i * 4] = v >> 16; px[i * 4 + 1] = v >> 8; px[i * 4 + 2] = v; px[i * 4 + 3] = v >> 24; }
    post([px = std::move(px), w, h] { gIa.panelTex = gRenderer.makeTexture(w, h, px.data(), gIa.panelTex); gIa.panelW = w; gIa.panelH = h; });
}

// ---- preview (GLSurfaceView thread) ----
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativePreviewInit(JNIEnv*, jobject) {
    gRenderer.contextLost(); gRenderer.init(); gRendererReady = true; gIa.panelTex = 0; hookCallbacks();
}
/** Draws the preview. (dx, dy): drag in radians; touch (tx, ty) in -1..1 with `down`; `menu` = next document button. */
JNIEXPORT void JNICALL Java_app_mobilecad_xr_XrActivity_nativePreviewDraw(JNIEnv*, jobject, jint w, jint h, jfloat dx, jfloat dy, jfloat zoom,
                                                                          jfloat tx, jfloat ty, jboolean touching, jboolean menu) {
    drainQueue();
    const Pose head{V3{0, 1.6f, 0}, Q{}, 1};
    if (gPlace) { gIa.placeModel(head); gPlace = false; }
    gPv.yaw += dx; gPv.pitch = std::max(-1.4f, std::min(1.4f, gPv.pitch + dy)); gPv.dist = std::max(0.2f, std::min(4.f, gPv.dist * zoom));
    // camera orbits the model's centre
    V3 c = gIa.scene.empty() ? V3{0, 1.45f, -0.7f} : apply(gIa.model, gIa.scene.center());
    Q rot = qnorm(axisAngle({0, 1, 0}, gPv.yaw) * axisAngle({1, 0, 0}, gPv.pitch));
    Pose eye{c + rotate(rot, V3{0, 0, gPv.dist}), rot, 1};
    float aspect = h > 0 ? (float)w / h : 1.f, fy = 0.55f, fx = std::atan(std::tan(fy) * aspect);
    M4 vp = mul(projectionFov(-fx, fx, fy, -fy, 0.02f, 50.f), viewFrom(eye));
    // the touch acts as the right controller: a ray from the eye through the touched point
    HandInput hands[2];
    V3 dir = norm(rotate(rot, V3{tx * std::tan(fx), ty * std::tan(fy), -1}));
    V3 z = dir * -1.f, xa = norm(cross(V3{0, 1, 0}, z)), ya = cross(z, xa);
    float tr = xa.x + ya.y + z.z; Q q{};
    if (tr > 0) { float s = std::sqrt(tr + 1) * 2; q = {(ya.z - z.y) / s, (z.x - xa.z) / s, (xa.y - ya.x) / s, s / 4}; }
    hands[1].active = true; hands[1].aim = {eye.pos + V3{0, -0.001f, 0}, qnorm(q), 1}; hands[1].grip = hands[1].aim;
    hands[1].select = touching ? 1.f : 0.f; hands[1].menu = menu;
    gIa.update(hands, 1.f / 60.f);
    gRenderer.gamma = 1.f; gRenderer.beginView(0, 0, 0, w, h, true);
    gIa.render(gRenderer, vp, eye.pos);
}

}  // extern "C"
