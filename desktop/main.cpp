// MobileCAD XR, desktop build: the same XR core as the Quest app, running against a Linux OpenXR runtime (Monado - with its
// simulated "qwerty" headset in sim/). Loads scene bundles (.mcxr, exported from MobileCAD by tools/export_scenes.py) and
// mirrors the left eye into a window. Instead of MobileCAD's Android UI (which only exists on Android) it shows a small
// stand-in panel with real buttons, so pointing at / clicking the UI panel can be tested here too.
//
//   mobilecad-xr-desktop [--autotest] [--true-size] [--screenshot out.ppm --frames N] scene.mcxr|dir ...
#include <dirent.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "../core/canvas.h"
#include "../core/xr_session.h"

using namespace mcxr;

typedef GLXContext (*CreateContextAttribs)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

struct Button { const char* label; int x0, y0, x1, y1; };
static const Button kButtons[] = {{"Next file", 80, 560, 760, 720}, {"Clear selection", 840, 560, 1520, 720}, {"Recenter", 80, 780, 760, 940}};

static void writePpm(const char* path, GLuint tex, int w, int h) {
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::vector<uint8_t> px(w * h * 4); glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo);
    FILE* f = std::fopen(path, "wb"); if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) for (int x = 0; x < w; ++x) std::fwrite(&px[(y * w + x) * 4], 1, 3, f);
    std::fclose(f); std::fprintf(stderr, "mcxr: wrote %s\n", path);
}

/** Orientation whose -Z points along `d` (for scripted aim poses). */
static Q lookRot(V3 d) {
    V3 z = norm(d * -1.f), x = norm(cross(V3{0, 1, 0}, z)), y = cross(z, x);
    float tr = x.x + y.y + z.z;
    if (tr > 0) { float s = std::sqrt(tr + 1) * 2; return qnorm(Q{(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4}); }
    return axisAngle(V3{0, 1, 0}, 3.14159f);
}

int main(int argc, char** argv) {
    std::vector<std::string> files; const char* shot = nullptr; int shotFrames = 120; bool autotest = false, startTrue = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--frames" && i + 1 < argc) shotFrames = std::atoi(argv[++i]);
        else if (a == "--autotest") autotest = true;
        else if (a == "--true-size") startTrue = true;
        else if (DIR* d = opendir(a.c_str())) {
            while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.size() > 5 && n.substr(n.size() - 5) == ".mcxr") files.push_back(a + "/" + n); }
            closedir(d);
        } else files.push_back(a);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::fprintf(stderr, "usage: %s [--autotest] [--screenshot out.ppm --frames N] scene.mcxr|dir ...\n", argv[0]); return 1; }

    // window + GL 3.3 core context (the OpenXR runtime renders our swapchains; this window only mirrors the left eye)
    Display* dpy = XOpenDisplay(nullptr); if (!dpy) { std::fprintf(stderr, "mcxr: no X display\n"); return 1; }
    int attrs[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                   GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, True, None};
    int nc = 0; GLXFBConfig* cfgs = glXChooseFBConfig(dpy, DefaultScreen(dpy), attrs, &nc);
    if (!cfgs || !nc) { std::fprintf(stderr, "mcxr: no GLX config\n"); return 1; }
    GLXFBConfig cfg = cfgs[0]; XFree(cfgs);
    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, cfg);
    XSetWindowAttributes swa{}; swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.event_mask = StructureNotifyMask | KeyPressMask;
    int WW = 960, WH = 540;
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, WW, WH, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, "MobileCAD XR - left eye"); XMapWindow(dpy, win);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False); XSetWMProtocols(dpy, win, &wmDelete, 1);
    auto create = (CreateContextAttribs)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    int ctxAttrs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3, GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
    GLXContext ctx = create ? create(dpy, cfg, nullptr, True, ctxAttrs) : nullptr;
    if (!ctx) { std::fprintf(stderr, "mcxr: no GL 3.3 context\n"); return 1; }
    glXMakeCurrent(dpy, win, ctx);
    std::fprintf(stderr, "mcxr: GL %s / %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    Renderer renderer; renderer.init();
    Interaction ia; size_t current = 0; Headset hs; bool placePanel = true; int loads = 0;
    int hot = -1, pressed = -1;                                       // hovered / pressed button
    std::string lastPick;
    auto drawPanel = [&] {
        Canvas c(ia.panel.pxW, ia.panel.pxH);
        c.rect(0, 0, c.w, c.h, 245, 247, 250);
        c.rect(0, 0, c.w, 120, 255, 255, 255); c.text(60, 28, "MobileCAD XR", 4, 22, 32, 43);
        c.text(60, 170, ia.scene.title + "  (" + std::to_string(current + 1) + "/" + std::to_string(files.size()) + ")", 4, 22, 32, 43);
        c.text(60, 250, std::to_string(ia.scene.meshes.size()) + " objects", 3, 95, 107, 119);
        c.text(60, 330, ia.selected.empty() ? "Click a face or edge to select it" : ("Selected: " + lastPick + (ia.selected.size() > 1 ? " +" + std::to_string(ia.selected.size() - 1) : "")), 3, 42, 111, 201);
        c.text(60, 420, "Desktop stand-in: the Quest app shows MobileCAD's full UI here", 2, 138, 150, 163);
        for (int b = 0; b < 3; ++b) {
            const Button& k = kButtons[b]; bool on = b == hot;
            uint8_t r = pressed == b ? 22 : on ? 58 : 42, g = pressed == b ? 81 : on ? 125 : 111, bl = pressed == b ? 160 : on ? 221 : 201;
            c.rect(k.x0, k.y0, k.x1, k.y1, r, g, bl, 24);
            int tw = (int)std::strlen(k.label) * 8 * 4; c.text((k.x0 + k.x1 - tw) / 2, (k.y0 + k.y1) / 2 - 32, k.label, 4, 255, 255, 255);
        }
        ia.panel.tex = renderer.makeTexture(c.w, c.h, c.px.data(), ia.panel.tex);
    };
    auto load = [&](size_t i) {
        current = i % files.size(); ++loads;
        if (!ia.scene.loadBundle(files[current])) std::fprintf(stderr, "mcxr: cannot read %s\n", files[current].c_str());
        std::fprintf(stderr, "mcxr: loaded %s (%zu objects)\n", ia.scene.title.c_str(), ia.scene.meshes.size());
        hs.needsPlacement = true; ia.selected.clear(); lastPick.clear(); drawPanel();
    };
    auto buttonAt = [&](float x, float y) {
        for (int b = 0; b < 3; ++b) if (x >= kButtons[b].x0 && x < kButtons[b].x1 && y >= kButtons[b].y0 && y < kButtons[b].y1) return b;
        return -1;
    };
    int panelClicks = 0;
    ia.panel.visible = true;
    ia.onPick = [&](const ElemRef& e, V3) {
        if (e.valid()) lastPick = ia.scene.meshes[e.mesh].name + (e.kind == 'E' ? " Edge" : " Face") + std::to_string(e.id);
        std::fprintf(stderr, "mcxr: pick %s\n", e.valid() ? lastPick.c_str() : "(nothing)");
        drawPanel();
    };
    ia.onPanelPointer = [&](int action, float x, float y) {
        int b = buttonAt(x, y);
        if (action == PanelHover || action == PanelMove) { if (b != hot) { hot = b; drawPanel(); } }
        else if (action == PanelLeave) { hot = -1; drawPanel(); }
        else if (action == PanelDown) { pressed = b; drawPanel(); }
        else if (action == PanelUp) {
            int was = pressed; pressed = -1;
            if (was >= 0 && was == b) {
                ++panelClicks; std::fprintf(stderr, "mcxr: panel button \"%s\"\n", kButtons[b].label);
                if (b == 0) load(current + 1);
                else if (b == 1) { ia.selected.clear(); lastPick.clear(); }
                else if (b == 2) { hs.needsPlacement = true; placePanel = true; }
            }
            drawPanel();
        }
    };
    ia.onMenu = [&] { load(current + 1); };
    load(0);

    PlatformInit p; p.display = dpy; p.fbConfig = cfg; p.drawable = win; p.context = ctx;
    std::string err;
    if (!hs.create(p, "MobileCAD XR", &err)) { std::fprintf(stderr, "mcxr: %s (is monado-service running?)\n", err.c_str()); return 2; }
    std::fprintf(stderr, "mcxr: OpenXR runtime \"%s\", system \"%s\"\n", hs.runtimeName.c_str(), hs.systemName.c_str());
    hs.onPlaced = [&](const Pose& head) { if (placePanel) { ia.placePanel(head); placePanel = false; } };

    // --autotest: a scripted right hand, checked through the same callbacks a user triggers (the runtime still drives the
    // session and the rendering): 1) click the model = pick  2) grab and carry it 25 cm  3) click the panel's "Next file"
    // 4) click the scale badge: true size (1 mm = 1 mm)  5) click it again: back to the fitted view
    struct Step { const char* what; bool ok = false; };
    std::vector<Step> steps{{"pick a face/edge on the model"}, {"grab + move the model 25 cm"}, {"click the panel's Next file button"},
                            {"scale badge: true size 1:1"}, {"scale badge: back to fit view"}};
    float fitScale = 0; V3 movedFrom{}; bool movedSet = false, frozen = false, autoDone = false, clicked = false; Pose frozenAim; size_t docBefore = 0;
    if (autotest) {
        hs.inputOverride = [&](HandInput h[2], const Pose& head, double t) {
            if (ia.scene.empty() || t < 1.0) return;
            HandInput& r = h[1]; r = {}; r.active = true;
            V3 from = head.pos + V3{0.15f, -0.25f, 0};
            if (t < 4.2) {
                V3 target = apply(ia.model, ia.scene.center());
                r.aim = {from, lookRot(target - from), 1};
                if (t > 2.4) { if (!frozen) { frozenAim = r.aim; frozen = true; } r.aim = frozenAim; }   // a real hand keeps its angle
                r.grip = r.aim;
                if (t > 1.5 && !clicked) { r.select = 1; clicked = true; }       // one frame, however slow the frames are
                if (t > 2.5 && t < 4.0) {
                    if (!movedSet) { movedFrom = ia.model.pos; movedSet = true; }
                    r.squeeze = 1; float k = (float)std::min(1.0, (t - 2.6) / 1.2); if (k > 0) { r.grip.pos = frozenAim.pos + V3{0.25f * k, 0, 0}; r.aim.pos = r.grip.pos; }
                }
                if (t > 4.0 && !steps[1].ok) { float m = ia.model.pos.x - movedFrom.x; steps[1].ok = m > 0.2f && m < 0.3f; std::fprintf(stderr, "mcxr autotest: model moved %.3f m\n", m); }
                if (t > 1.7 && !steps[0].ok) steps[0].ok = !ia.selected.empty();
            } else if (t > 5.5) {
                // the scale badge under the panel: click it twice
                r.aim = {from, lookRot(ia.badgeButton() - from), 1}; r.grip = r.aim;
                if (t > 5.6 && !fitScale) fitScale = ia.model.scale;
                if ((t > 5.9 && t < 6.0) || (t > 6.6 && t < 6.7)) r.select = 1;
                if (t > 6.3 && !steps[3].ok) {
                    steps[3].ok = ia.trueScale && std::fabs(ia.model.scale - 0.001f) < 1e-7f && ia.scaleText() == "1:1";
                    std::fprintf(stderr, "mcxr autotest: true size, scale %s\n", ia.scaleText().c_str());
                }
                if (t > 7.0 && !autoDone) {
                    steps[4].ok = !ia.trueScale && ia.model.scale == fitScale;
                    std::fprintf(stderr, "mcxr autotest: fit view, scale %s\n", ia.scaleText().c_str());
                    autoDone = true; bool all = true;
                    for (auto& s : steps) { std::fprintf(stderr, "mcxr autotest: %-36s %s\n", s.what, s.ok ? "PASS" : "FAIL"); all &= s.ok; }
                    std::fprintf(stderr, "mcxr autotest: %s\n", all ? "PASS" : "FAIL");
                }
            } else {
                // aim at the centre of "Next file" on the panel, then click
                const Button& b = kButtons[0];
                float u = (b.x0 + b.x1) * 0.5f / ia.panel.pxW, v = (b.y0 + b.y1) * 0.5f / ia.panel.pxH;
                V3 target = apply(Pose{ia.panel.pose.pos, ia.panel.pose.rot, 1}, V3{(u - 0.5f) * ia.panel.width, (0.5f - v) * ia.panel.height, 0});
                r.aim = {from, lookRot(target - from), 1}; r.grip = r.aim;
                if (t > 4.4 && !docBefore) docBefore = (size_t)loads;
                if (t > 4.8 && t < 4.9) r.select = 1;
                if (t > 5.4) steps[2].ok = panelClicks == 1 && (size_t)loads == docBefore + 1;    // the button loaded the next file
            }
        };
    }
    bool quit = false; int rendered = 0;
    while (!quit) {
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wmDelete) quit = true;
            if (ev.type == ConfigureNotify) { WW = ev.xconfigure.width; WH = ev.xconfigure.height; }
        }
        hs.pollEvents(&quit);
        if (!hs.running()) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        if (hs.frame(ia, renderer)) ++rendered;
        if (startTrue && rendered == 2) ia.setTrueScale(true);           // after placement and a first render (viewer known)
        if (hs.mirrorTex) {                                             // mirror the left eye, keeping its aspect
            GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hs.mirrorTex, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); glViewport(0, 0, WW, WH); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
            float s = std::min((float)WW / hs.eyeW, (float)WH / hs.eyeH); int w = (int)(hs.eyeW * s), h = (int)(hs.eyeH * s);
            glBlitFramebuffer(0, 0, hs.eyeW, hs.eyeH, (WW - w) / 2, (WH - h) / 2, (WW - w) / 2 + w, (WH - h) / 2 + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glDeleteFramebuffers(1, &fbo);
            glXSwapBuffers(dpy, win);
            if (shot && rendered >= shotFrames && (!autotest || autoDone)) { writePpm(shot, hs.mirrorTex, hs.eyeW, hs.eyeH); quit = true; }
            if (autotest && autoDone && !shot) quit = true;
        }
    }
    hs.destroy();
    glXMakeCurrent(dpy, None, nullptr); glXDestroyContext(dpy, ctx); XDestroyWindow(dpy, win); XCloseDisplay(dpy);
    if (autotest) { for (auto& s : steps) if (!s.ok) return 3; }
    return 0;
}
