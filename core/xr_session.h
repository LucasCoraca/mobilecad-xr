// OpenXR plumbing: instance, system, session, stereo swapchains, controller actions and the frame loop. Built for
// Android headsets (OpenGL ES, XR_KHR_opengl_es_enable) and for Linux against Monado (OpenGL/Xlib, XR_KHR_opengl_enable).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "gl.h"
#include "interaction.h"
#include "renderer.h"

#ifdef __ANDROID__
#include <EGL/egl.h>
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#else
#include <X11/Xlib.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace mcxr {

struct PlatformInit {
#ifdef __ANDROID__
    JavaVM* vm = nullptr; jobject activity = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY; EGLConfig config = nullptr; EGLContext context = EGL_NO_CONTEXT;
#else
    Display* display = nullptr; GLXFBConfig fbConfig = nullptr; GLXDrawable drawable = 0; GLXContext context = nullptr;
#endif
};

class Headset {
public:
    /** Creates the instance + session. False (with the reason in `error`) when there is no OpenXR runtime / headset. */
    bool create(const PlatformInit& p, const char* appName, std::string* error);
    /** Handles runtime events; sets `quit` when the session ends or the runtime asks the app to exit. */
    void pollEvents(bool* quit);
    bool running() const { return sessionRunning; }
    /** Renders one frame (both eyes) when the runtime wants one; returns false if nothing was rendered. */
    bool frame(Interaction& ia, Renderer& r);
    void destroy();

    std::string runtimeName, systemName;
    bool needsPlacement = true;      // place the model in front of the viewer on the first tracked frame
    // the left eye of the last frame, for a desktop mirror window
    GLuint mirrorTex = 0; int eyeW = 0, eyeH = 0;
    Pose head; bool headValid = false;
    /** Test hook: replaces the controllers' input after it is read (desktop --autotest); t = seconds since the first frame. */
    std::function<void(HandInput hands[2], const Pose& head, double t)> inputOverride;
    /** Called with the head pose while `needsPlacement` is set (the app places its panels; the model is placed here). */
    std::function<void(const Pose& head)> onPlaced;

private:
    XrInstance instance = XR_NULL_HANDLE; XrSystemId system = XR_NULL_SYSTEM_ID; XrSession session = XR_NULL_HANDLE;
    XrSpace appSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN; bool sessionRunning = false;
    bool stage = false;              // floor-level tracking space
    struct Eye { XrSwapchain swapchain = XR_NULL_HANDLE; std::vector<GLuint> images; GLuint depth = 0, fbo = 0; int w = 0, h = 0; };
    Eye eyes[2]; int64_t colorFormat = 0; bool srgb = false;
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction aAim = XR_NULL_HANDLE, aGrip = XR_NULL_HANDLE, aSelect = XR_NULL_HANDLE, aSqueeze = XR_NULL_HANDLE, aStick = XR_NULL_HANDLE, aMenu = XR_NULL_HANDLE, aContext = XR_NULL_HANDLE;
    XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH}; XrSpace aimSpace[2] = {}, gripSpace[2] = {};
    XrTime lastTime = 0, firstTime = 0;
    bool makeActions();
    bool makeSwapchains();
    void readHands(XrTime t, HandInput out[2]);
};

/** OpenXR pose -> core pose. */
inline Pose toPose(const XrPosef& p) { return {V3{p.position.x, p.position.y, p.position.z}, Q{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, 1}; }

}  // namespace mcxr
