#include "xr_session.h"
#include <cstdio>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "mcxr", __VA_ARGS__)
#else
#define LOG(...) (std::fprintf(stderr, "mcxr: " __VA_ARGS__), std::fputc('\n', stderr))
#endif

namespace mcxr {

#define XRCHECK(call) do { XrResult r_ = (call); if (XR_FAILED(r_)) { char s_[XR_MAX_RESULT_STRING_SIZE] = "?"; \
    if (instance) xrResultToString(instance, r_, s_); LOG("%s failed: %s (%d)", #call, s_, (int)r_); return false; } } while (0)

static bool hasExtension(const std::vector<XrExtensionProperties>& exts, const char* name) {
    for (auto& e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool Headset::create(const PlatformInit& p, const char* appName, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; LOG("%s", why); destroy(); return false; };
#ifdef __ANDROID__
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader)) && initLoader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR}; li.applicationVM = p.vm; li.applicationContext = p.activity;
        initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&li);
    }
    const char* gfxExt = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
#else
    const char* gfxExt = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
#endif
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) return fail("No OpenXR runtime is installed");
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    if (!hasExtension(exts, gfxExt)) return fail("The OpenXR runtime has no OpenGL support");
    std::vector<const char*> enable{gfxExt};
#ifdef __ANDROID__
    if (hasExtension(exts, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) enable.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR}; android.applicationVM = p.vm; android.applicationActivity = p.activity;
#endif
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
    ci.next = &android;
#endif
    std::snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", appName);
    std::snprintf(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "MobileCAD XR");
    ci.applicationInfo.applicationVersion = 1; ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = (uint32_t)enable.size(); ci.enabledExtensionNames = enable.data();
    if (XR_FAILED(xrCreateInstance(&ci, &instance))) return fail("Could not start OpenXR");
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES}; xrGetInstanceProperties(instance, &ip); runtimeName = ip.runtimeName;

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO}; sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(instance, &sg, &system))) return fail("No headset is connected");
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES}; xrGetSystemProperties(instance, system, &sp); systemName = sp.systemName;

    // graphics requirements must be queried before the session (spec); the GL context is the caller's
#ifdef __ANDROID__
    PFN_xrGetOpenGLESGraphicsRequirementsKHR req = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&req);
    XrGraphicsRequirementsOpenGLESKHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR}; if (req) req(instance, system, &gr);
    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = p.display; binding.config = p.config; binding.context = p.context;
#else
    PFN_xrGetOpenGLGraphicsRequirementsKHR req = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&req);
    XrGraphicsRequirementsOpenGLKHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR}; if (req) req(instance, system, &gr);
    XrGraphicsBindingOpenGLXlibKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
    binding.xDisplay = p.display; binding.glxFBConfig = p.fbConfig; binding.glxDrawable = p.drawable; binding.glxContext = p.context;
    binding.visualid = 0;
#endif
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; sci.next = &binding; sci.systemId = system;
    if (XR_FAILED(xrCreateSession(instance, &sci, &session))) return fail("Could not create the XR session");

    // stage (floor-level) space when the runtime has one, else local (head-level origin)
    uint32_t ns = 0; xrEnumerateReferenceSpaces(session, 0, &ns, nullptr);
    std::vector<XrReferenceSpaceType> spaces(ns); xrEnumerateReferenceSpaces(session, ns, &ns, spaces.data());
    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (auto s : spaces) if (s == XR_REFERENCE_SPACE_TYPE_STAGE) type = XR_REFERENCE_SPACE_TYPE_STAGE;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = type; if (XR_FAILED(xrCreateReferenceSpace(session, &rs, &appSpace))) return fail("No tracking space");
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; xrCreateReferenceSpace(session, &rs, &viewSpace);
    if (!makeSwapchains()) return fail("Could not create the eye images");
    if (!makeActions()) return fail("Could not set up the controllers");
    LOG("runtime %s, system %s, %dx%d per eye, %s space", runtimeName.c_str(), systemName.c_str(), eyes[0].w, eyes[0].h,
        type == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : "local");
    return true;
}

bool Headset::makeSwapchains() {
    uint32_t nv = 0; xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nv, nullptr);
    std::vector<XrViewConfigurationView> views(nv, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XRCHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, nv, &nv, views.data()));
    if (nv < 2) return false;
    uint32_t nf = 0; xrEnumerateSwapchainFormats(session, 0, &nf, nullptr);
    std::vector<int64_t> fmts(nf); xrEnumerateSwapchainFormats(session, nf, &nf, fmts.data());
    colorFormat = 0;
    for (int64_t want : {(int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8})
        for (int64_t f : fmts) if (!colorFormat && f == want) { colorFormat = f; srgb = (f == GL_SRGB8_ALPHA8); }
    if (!colorFormat && nf) colorFormat = fmts[0];
    for (int e = 0; e < 2; ++e) {
        Eye& eye = eyes[e]; eye.w = (int)views[e].recommendedImageRectWidth; eye.h = (int)views[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = colorFormat; sc.sampleCount = 1; sc.width = eye.w; sc.height = eye.h; sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
        XRCHECK(xrCreateSwapchain(session, &sc, &eye.swapchain));
        uint32_t ni = 0; xrEnumerateSwapchainImages(eye.swapchain, 0, &ni, nullptr);
#ifdef __ANDROID__
        std::vector<XrSwapchainImageOpenGLESKHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#else
        std::vector<XrSwapchainImageOpenGLKHR> imgs(ni, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
#endif
        XRCHECK(xrEnumerateSwapchainImages(eye.swapchain, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data()));
        for (auto& i : imgs) eye.images.push_back(i.image);
        glGenRenderbuffers(1, &eye.depth); glBindRenderbuffer(GL_RENDERBUFFER, eye.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, eye.w, eye.h);
        glGenFramebuffers(1, &eye.fbo);
    }
    eyeW = eyes[0].w; eyeH = eyes[0].h;
    return true;
}

bool Headset::makeActions() {
    XrActionSetCreateInfo as{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(as.actionSetName, "cad"); std::strcpy(as.localizedActionSetName, "CAD");
    XRCHECK(xrCreateActionSet(instance, &as, &actionSet));
    xrStringToPath(instance, "/user/hand/left", &hands[0]); xrStringToPath(instance, "/user/hand/right", &hands[1]);
    auto make = [&](XrAction* a, const char* name, const char* label, XrActionType t) {
        XrActionCreateInfo ac{XR_TYPE_ACTION_CREATE_INFO}; ac.actionType = t; std::strcpy(ac.actionName, name); std::strcpy(ac.localizedActionName, label);
        ac.countSubactionPaths = 2; ac.subactionPaths = hands; return XR_SUCCEEDED(xrCreateAction(actionSet, &ac, a));
    };
    if (!make(&aAim, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT) || !make(&aGrip, "grip", "Hand", XR_ACTION_TYPE_POSE_INPUT) ||
        !make(&aSelect, "select", "Select / grab", XR_ACTION_TYPE_FLOAT_INPUT) || !make(&aSqueeze, "squeeze", "Grab", XR_ACTION_TYPE_FLOAT_INPUT) ||
        !make(&aStick, "stick", "Scale / turn", XR_ACTION_TYPE_VECTOR2F_INPUT) || !make(&aMenu, "menu", "Next document", XR_ACTION_TYPE_BOOLEAN_INPUT))
        return false;
    // suggested bindings per controller; a profile the runtime does not know is simply skipped
    struct B { XrAction a; const char* path; };
    auto suggest = [&](const char* profile, std::vector<B> bs) {
        XrPath prof; xrStringToPath(instance, profile, &prof);
        std::vector<XrActionSuggestedBinding> v;
        for (auto& b : bs) { XrPath p; if (XR_SUCCEEDED(xrStringToPath(instance, b.path, &p))) v.push_back({b.a, p}); }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = prof; sb.countSuggestedBindings = (uint32_t)v.size(); sb.suggestedBindings = v.data();
        XrResult r = xrSuggestInteractionProfileBindings(instance, &sb);
        if (XR_FAILED(r)) LOG("bindings for %s not accepted (%d)", profile, (int)r);
    };
    const char* L = "/user/hand/left/input/"; const char* R = "/user/hand/right/input/";
    std::vector<std::string> s; s.reserve(64);    // backing storage for the path strings (no reallocation: pointers stay valid)
    auto pr = [&](const std::string& x) { s.push_back(x); return s.back().c_str(); };
    suggest("/interaction_profiles/khr/simple_controller", {
        {aAim, pr(std::string(L) + "aim/pose")}, {aAim, pr(std::string(R) + "aim/pose")},
        {aGrip, pr(std::string(L) + "grip/pose")}, {aGrip, pr(std::string(R) + "grip/pose")},
        {aSelect, pr(std::string(L) + "select/click")}, {aSelect, pr(std::string(R) + "select/click")},
        {aMenu, pr(std::string(L) + "menu/click")}, {aMenu, pr(std::string(R) + "menu/click")}});
    suggest("/interaction_profiles/oculus/touch_controller", {
        {aAim, pr(std::string(L) + "aim/pose")}, {aAim, pr(std::string(R) + "aim/pose")},
        {aGrip, pr(std::string(L) + "grip/pose")}, {aGrip, pr(std::string(R) + "grip/pose")},
        {aSelect, pr(std::string(L) + "trigger/value")}, {aSelect, pr(std::string(R) + "trigger/value")},
        {aSqueeze, pr(std::string(L) + "squeeze/value")}, {aSqueeze, pr(std::string(R) + "squeeze/value")},
        {aStick, pr(std::string(L) + "thumbstick")}, {aStick, pr(std::string(R) + "thumbstick")},
        {aMenu, pr(std::string(L) + "x/click")}, {aMenu, pr(std::string(R) + "a/click")}});
    suggest("/interaction_profiles/microsoft/motion_controller", {
        {aAim, pr(std::string(L) + "aim/pose")}, {aAim, pr(std::string(R) + "aim/pose")},
        {aGrip, pr(std::string(L) + "grip/pose")}, {aGrip, pr(std::string(R) + "grip/pose")},
        {aSelect, pr(std::string(L) + "trigger/value")}, {aSelect, pr(std::string(R) + "trigger/value")},
        {aSqueeze, pr(std::string(L) + "squeeze/click")}, {aSqueeze, pr(std::string(R) + "squeeze/click")},
        {aStick, pr(std::string(L) + "thumbstick")}, {aStick, pr(std::string(R) + "thumbstick")},
        {aMenu, pr(std::string(L) + "menu/click")}, {aMenu, pr(std::string(R) + "menu/click")}});
    suggest("/interaction_profiles/valve/index_controller", {
        {aAim, pr(std::string(L) + "aim/pose")}, {aAim, pr(std::string(R) + "aim/pose")},
        {aGrip, pr(std::string(L) + "grip/pose")}, {aGrip, pr(std::string(R) + "grip/pose")},
        {aSelect, pr(std::string(L) + "trigger/value")}, {aSelect, pr(std::string(R) + "trigger/value")},
        {aSqueeze, pr(std::string(L) + "squeeze/value")}, {aSqueeze, pr(std::string(R) + "squeeze/value")},
        {aStick, pr(std::string(L) + "thumbstick")}, {aStick, pr(std::string(R) + "thumbstick")},
        {aMenu, pr(std::string(L) + "b/click")}, {aMenu, pr(std::string(R) + "b/click")}});
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO}; sc.subactionPath = hands[i]; sc.poseInActionSpace.orientation.w = 1;
        sc.action = aAim; XRCHECK(xrCreateActionSpace(session, &sc, &aimSpace[i]));
        sc.action = aGrip; XRCHECK(xrCreateActionSpace(session, &sc, &gripSpace[i]));
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; at.countActionSets = 1; at.actionSets = &actionSet;
    XRCHECK(xrAttachSessionActionSets(session, &at));
    return true;
}

void Headset::pollEvents(bool* quit) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (instance && xrPollEvent(instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sc = (XrEventDataSessionStateChanged*)&ev; state = sc->state;
            LOG("session state %d", (int)state);
            if (state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(session, &bi))) sessionRunning = true;
            } else if (state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session); sessionRunning = false;
            } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                if (quit) *quit = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            if (quit) *quit = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void Headset::readHands(XrTime t, HandInput out[2]) {
    XrActiveActionSet active{actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO}; si.countActiveActionSets = 1; si.activeActionSets = &active;
    xrSyncActions(session, &si);
    for (int i = 0; i < 2; ++i) {
        HandInput& h = out[i]; h = {};
        XrSpaceLocation la{XR_TYPE_SPACE_LOCATION}, lg{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(aimSpace[i], appSpace, t, &la); xrLocateSpace(gripSpace[i], appSpace, t, &lg);
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        h.active = (la.locationFlags & need) == need;
        h.aim = toPose(la.pose); h.grip = (lg.locationFlags & need) == need ? toPose(lg.pose) : h.aim;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.subactionPath = hands[i];
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = aSelect; if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive) h.select = f.currentState;
        f = {XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = aSqueeze; if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &f)) && f.isActive) h.squeeze = f.currentState;
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = aStick; if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &v)) && v.isActive) { h.stickX = v.currentState.x; h.stickY = v.currentState.y; }
        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = aMenu; if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &b)) && b.isActive) h.menu = b.currentState;
    }
}

bool Headset::frame(Interaction& ia, Renderer& r) {
    if (!sessionRunning) return false;
    XrFrameState fs{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(session, &wi, &fs))) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(session, &bi);
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool rendered = false;
    if (fs.shouldRender) {
        float dt = lastTime ? (float)((fs.predictedDisplayTime - lastTime) * 1e-9) : 0.f; lastTime = fs.predictedDisplayTime;
        if (dt < 0 || dt > 0.1f) dt = 1.f / 72.f;
        XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(viewSpace, appSpace, fs.predictedDisplayTime, &hl);
        headValid = (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (headValid) head = toPose(hl.pose);
        if (needsPlacement && headValid && !ia.scene.empty()) { ia.placeModel(head); needsPlacement = false; }
        HandInput hin[2]; readHands(fs.predictedDisplayTime, hin);
        if (!firstTime) firstTime = fs.predictedDisplayTime;
        if (inputOverride) inputOverride(hin, head, (fs.predictedDisplayTime - firstTime) * 1e-9);
        ia.update(hin, dt);

        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO}; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime; li.space = appSpace;
        XrViewState vs{XR_TYPE_VIEW_STATE}; XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t nv = 0;
        if (XR_SUCCEEDED(xrLocateViews(session, &li, &vs, 2, &nv, views)) && nv == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            r.gamma = srgb ? 2.2f : 1.f;
#ifndef __ANDROID__
            if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
#endif
            for (int e = 0; e < 2; ++e) {
                Eye& eye = eyes[e]; uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; xrAcquireSwapchainImage(eye.swapchain, &ai, &idx);
                XrSwapchainImageWaitInfo wti{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wti.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(eye.swapchain, &wti);
                glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye.images[idx], 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, eye.depth);
                Pose ep = toPose(views[e].pose);
                M4 proj = projectionFov(views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown, 0.02f, 50.f);
                M4 vp = mul(proj, viewFrom(ep));
                r.beginView(eye.fbo, 0, 0, eye.w, eye.h, true);
                ia.render(r, vp, ep.pos);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                if (e == 0) mirrorTex = eye.images[idx];
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(eye.swapchain, &ri);
                pv[e].pose = views[e].pose; pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = eye.swapchain; pv[e].subImage.imageRect = {{0, 0}, {eye.w, eye.h}};
            }
#ifndef __ANDROID__
            glDisable(GL_FRAMEBUFFER_SRGB);
#endif
            layer.space = appSpace; layer.viewCount = 2; layer.views = pv; rendered = true;
        }
    }
    const XrCompositionLayerBaseHeader* layers[] = {(XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO}; ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; ei.layerCount = rendered ? 1 : 0; ei.layers = layers;
    xrEndFrame(session, &ei);
    return rendered;
}

void Headset::destroy() {
    for (Eye& e : eyes) { if (e.swapchain) xrDestroySwapchain(e.swapchain); e.swapchain = XR_NULL_HANDLE; }
    for (int i = 0; i < 2; ++i) { if (aimSpace[i]) xrDestroySpace(aimSpace[i]); if (gripSpace[i]) xrDestroySpace(gripSpace[i]); aimSpace[i] = gripSpace[i] = XR_NULL_HANDLE; }
    if (appSpace) xrDestroySpace(appSpace); if (viewSpace) xrDestroySpace(viewSpace); appSpace = viewSpace = XR_NULL_HANDLE;
    if (actionSet) xrDestroyActionSet(actionSet); actionSet = XR_NULL_HANDLE;
    if (session) xrDestroySession(session); session = XR_NULL_HANDLE;
    if (instance) xrDestroyInstance(instance); instance = XR_NULL_HANDLE;
    sessionRunning = false;
}

}  // namespace mcxr
