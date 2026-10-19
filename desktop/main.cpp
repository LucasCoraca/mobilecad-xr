// MobileCAD XR, desktop build: the same XR core as the Quest app, running against a Linux OpenXR runtime (Monado - with its
// simulated "qwerty" headset in sim/). Loads scene bundles (.mcxr, exported from MobileCAD by tools/export_scenes.py) and
// mirrors the left eye into a window.
//
//   mobilecad-xr-desktop [--screenshot out.ppm --frames N] scene.mcxr|dir ...
#include <dirent.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include "font8x16.h"
#include "../core/xr_session.h"

using namespace mcxr;

typedef GLXContext (*CreateContextAttribs)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

/** Info panel: dark rounded card with lines of text (8x16 font, scaled 2x). */
static std::vector<uint8_t> panel(const std::vector<std::string>& lines, int W, int H) {
    std::vector<uint8_t> px(W * H * 4, 0);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int dx = std::max({0, 16 - x, x - (W - 17)}), dy = std::max({0, 16 - y, y - (H - 17)});
        if (dx * dx + dy * dy > 256) continue;
        uint8_t* p = &px[(y * W + x) * 4]; p[0] = 24; p[1] = 28; p[2] = 34; p[3] = 235;
    }
    for (size_t li = 0; li < lines.size(); ++li) {
        int scale = li == 0 ? 3 : 2, y0 = li == 0 ? 18 : 30 + (int)li * 36;
        uint8_t col[3] = {li == 0 ? (uint8_t)255 : (uint8_t)200, li == 0 ? (uint8_t)255 : (uint8_t)208, li == 0 ? (uint8_t)255 : (uint8_t)216};
        int x0 = 24;
        for (char c : lines[li]) {
            if (c < 32 || c > 126) c = '?';
            for (int gy = 0; gy < 16; ++gy) for (int gx = 0; gx < 8; ++gx)
                if (kFont8x16[c - 32][gy] & (0x80 >> gx))
                    for (int sy = 0; sy < scale; ++sy) for (int sx = 0; sx < scale; ++sx) {
                        int x = x0 + gx * scale + sx, y = y0 + gy * scale + sy; if (x >= W || y >= H) continue;
                        uint8_t* p = &px[(y * W + x) * 4]; p[0] = col[0]; p[1] = col[1]; p[2] = col[2]; p[3] = 255;
                    }
            x0 += 8 * scale;
        }
    }
    return px;
}

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

int main(int argc, char** argv) {
    std::vector<std::string> files; const char* shot = nullptr; int shotFrames = 120; bool autotest = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--frames" && i + 1 < argc) shotFrames = std::atoi(argv[++i]);
        else if (a == "--autotest") autotest = true;
        else if (DIR* d = opendir(a.c_str())) {
            while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n.size() > 5 && n.substr(n.size() - 5) == ".mcxr") files.push_back(a + "/" + n); }
            closedir(d);
        } else files.push_back(a);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::fprintf(stderr, "usage: %s [--screenshot out.ppm --frames N] scene.mcxr|dir ...\n", argv[0]); return 1; }

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
    Interaction ia; size_t current = 0; Headset hs; std::string status;
    auto refreshPanel = [&](const std::string& sel) {
        std::vector<std::string> lines{ia.scene.title, std::to_string(ia.scene.meshes.size()) + " objects  (" + std::to_string(current + 1) + "/" + std::to_string(files.size()) + ")",
                                       sel.empty() ? "Click a face to select it" : "Selected: " + sel, "Hold trigger: move   Menu: next file"};
        auto px = panel(lines, 640, 240); ia.panelTex = renderer.makeTexture(640, 240, px.data(), ia.panelTex); ia.panelW = 640; ia.panelH = 240;
    };
    auto load = [&](size_t i) {
        current = i % files.size();
        if (!ia.scene.loadBundle(files[current])) std::fprintf(stderr, "mcxr: cannot read %s\n", files[current].c_str());
        std::fprintf(stderr, "mcxr: loaded %s (%zu objects)\n", ia.scene.title.c_str(), ia.scene.meshes.size());
        hs.needsPlacement = true; refreshPanel("");
    };
    ia.onSelect = [&](const std::string& obj, int face) {
        std::fprintf(stderr, "mcxr: select %s Face%d\n", obj.c_str(), face); refreshPanel(face ? obj + " Face" + std::to_string(face) : "");
    };
    ia.onNextDocument = [&] { load(current + 1); };
    load(0);

    PlatformInit p; p.display = dpy; p.fbConfig = cfg; p.drawable = win; p.context = ctx;
    std::string err;
    if (!hs.create(p, "MobileCAD XR", &err)) { std::fprintf(stderr, "mcxr: %s (is monado-service running?)\n", err.c_str()); return 2; }
    std::fprintf(stderr, "mcxr: OpenXR runtime \"%s\", system \"%s\"\n", hs.runtimeName.c_str(), hs.systemName.c_str());

    // --autotest: scripted right hand (aim at the model + click = select; then grab and carry it 25 cm to the right),
    // checked through the same callbacks a user triggers; the runtime still drives the session and the rendering
    int autoSelects = 0; V3 movedFrom{}; bool autoDone = false, autoOk = false, aimFrozen = false; Pose frozenAim;
    if (autotest) {
        ia.onSelect = [&, prev = ia.onSelect](const std::string& o, int f) { if (f > 0) ++autoSelects; prev(o, f); };
        hs.inputOverride = [&](HandInput h[2], const Pose& head, double t) {
            if (ia.scene.empty() || t < 1.0) return;
            V3 target = apply(ia.model, ia.scene.center());
            V3 from = head.pos + V3{0.15f, -0.25f, 0}; V3 d = norm(target - from);
            // orientation whose -Z points along d
            V3 z = d * -1.f, x = norm(cross(V3{0, 1, 0}, z)), y = cross(z, x);
            float tr = x.x + y.y + z.z; Q q;
            if (tr > 0) { float s = std::sqrt(tr + 1) * 2; q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4}; }
            else { q = axisAngle(V3{0, 1, 0}, 3.14159f); }
            HandInput& r = h[1]; r = {}; r.active = true; r.aim = {from, qnorm(q), 1};
            if (t > 2.4) { if (!aimFrozen) { frozenAim = r.aim; aimFrozen = true; } r.aim = frozenAim; }   // a real hand keeps its angle
            r.grip = r.aim;
            if (t > 1.5 && t < 1.6) r.select = 1;                                   // click: select the face under the ray
            if (t > 2.5 && t < 4.0) {                                               // grab and carry 25 cm to the right
                if (movedFrom.x == 0 && movedFrom.y == 0) movedFrom = ia.model.pos;
                r.squeeze = 1; float k = (float)std::min(1.0, (t - 2.6) / 1.2); if (k > 0) { r.grip.pos = frozenAim.pos + V3{0.25f * k, 0, 0}; r.aim.pos = r.grip.pos; }
            }
            if (t > 4.5 && !autoDone) {
                autoDone = true; float moved = ia.model.pos.x - movedFrom.x;
                autoOk = autoSelects > 0 && ia.selected.face > 0 && moved > 0.2f && moved < 0.3f;
                std::fprintf(stderr, "mcxr autotest: selected %s Face%d (%d select events), model moved %.3f m -> %s\n",
                             ia.selected.mesh >= 0 ? ia.scene.meshes[ia.selected.mesh].name.c_str() : "-", ia.selected.face, autoSelects, moved, autoOk ? "PASS" : "FAIL");
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
    if (autotest) return autoOk ? 0 : 3;
    glXMakeCurrent(dpy, None, nullptr); glXDestroyContext(dpy, ctx); XDestroyWindow(dpy, win); XCloseDisplay(dpy);
    return 0;
}
