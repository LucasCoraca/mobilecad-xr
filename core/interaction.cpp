#include "interaction.h"
#include <algorithm>
#include <cstdio>
#include "canvas.h"

namespace mcxr {

static V3 forward(const Pose& p) { return rotate(p.rot, V3{0, 0, -1}); }
static Pose unscaled(const Pose& p) { return Pose{p.pos, p.rot, 1}; }
static const float kTrueScale = 0.001f;              // scene mm -> world m
static const float kBadgeW = 0.42f, kBadgeH = 0.06f; static const int kBadgePx = 2000;   // badge size (m), pixels per metre
static Q facing(V3 from, V3 to) {                      // yaw-only rotation whose +Z points from `to` back to `from`
    V3 d = from - to; d.y = 0; if (len(d) < 1e-4f) d = {0, 0, 1};
    d = norm(d); return axisAngle({0, 1, 0}, std::atan2(d.x, d.z));
}

void Interaction::placeModel(const Pose& head) {
    if (scene.empty()) return;
    float r = std::max(scene.radius(), 1e-3f);
    V3 f = forward(head); f.y = 0; f = len(f) > 1e-3f ? norm(f) : V3{0, 0, -1};
    V3 right = cross(f, V3{0, 1, 0});
    V3 target = head.pos + f * 0.75f + right * 0.32f + V3{0, -0.2f, 0};        // to the right of the UI panel
    float yaw = std::atan2(-f.x, -f.z);
    // FreeCAD is Z-up, OpenXR is Y-up: turn the model upright, then face the viewer
    Q upright = axisAngle({1, 0, 0}, -1.5707963f);
    model = {V3{}, qnorm(axisAngle({0, 1, 0}, yaw) * upright), 0.2f / r};    // ~40 cm across
    model.pos = target - apply(model, scene.center());
    hover[0] = hover[1] = {};
    if (!hostSelection) selected.clear();
    trueScale = false; fitSaved = false;
}

float Interaction::lowestY() const {
    float low = 1e30f;
    for (const Mesh& m : scene.meshes) for (size_t i = 0; i + 2 < m.tris.size(); i += 7)
        low = std::min(low, apply(model, V3{m.tris[i], m.tris[i + 1], m.tris[i + 2]}).y);
    return low;
}

void Interaction::setTrueScale(bool on) {
    if (scene.empty() || on == trueScale) return;
    trueScale = on;
    if (!on) { if (fitSaved) model = fitPose; fitSaved = false; return; }
    fitPose = model; fitSaved = true;
    V3 c = apply(model, scene.center());                              // scale about the model's centre
    model.pos = c + (model.pos - c) * (kTrueScale / model.scale); model.scale = kTrueScale;
    float r = scene.radius() * kTrueScale;
    if (r > 0.45f) {
        // bigger than arm's reach: stand it on the floor, far enough in front of the viewer to walk around it
        V3 d = c - viewer; d.y = 0; d = len(d) > 1e-3f ? norm(d) : V3{0, 0, -1};
        V3 nc = viewer + d * (r + 0.4f); nc.y = c.y;
        model.pos = model.pos + (nc - c);
        model.pos.y += floorY - lowestY();
    } else {
        float low = lowestY(); if (low < floorY) model.pos.y += floorY - low;    // never below the floor
    }
}

std::string Interaction::scaleText() const {
    float k = model.scale / kTrueScale; char b[32];
    if (std::fabs(k - 1) < 0.005f) return "1:1";
    float v = k < 1 ? 1 / k : k;
    std::snprintf(b, sizeof b, v < 9.95f ? "%.1f" : "%.0f", v);
    std::string n = b; if (n.size() > 2 && n.compare(n.size() - 2, 2, ".0") == 0) n.resize(n.size() - 2);
    return k < 1 ? "1:" + n : n + ":1";
}

Pose Interaction::badgePose() const {                                  // hangs under the panel's right-hand corner
    Pose p{panel.pose.pos, panel.pose.rot, 1};
    p.pos = apply(p, V3{panel.width / 2 - kBadgeW / 2, -panel.height / 2 - 0.018f - kBadgeH / 2, 0});
    return p;
}

V3 Interaction::badgeButton() const { return apply(badgePose(), V3{kBadgeW / 2 - 0.078f, 0, 0}); }

void Interaction::placePanel(const Pose& head) {
    V3 f = forward(head); f.y = 0; f = len(f) > 1e-3f ? norm(f) : V3{0, 0, -1};
    V3 right = cross(f, V3{0, 1, 0});
    V3 c = head.pos + f * 0.95f - right * 0.42f + V3{0, -0.12f, 0};
    panel.pose = {c, facing(head.pos, c), 1};
    // tilt back a little, like a drafting table
    panel.pose.rot = qnorm(panel.pose.rot * axisAngle({1, 0, 0}, -0.12f));
}

/** Ray against a w x h rectangle centred on `rect` (local X right, Y up): distance and 0..1 coordinates (v down). */
static bool rectRay(const Pose& aim, const Pose& rect, float w, float h, float* t, float* u, float* v, bool clamp) {
    V3 n = rotate(rect.rot, {0, 0, 1}), d = forward(aim);
    float den = dot(d, n); if (std::fabs(den) < 1e-5f) return false;
    float tt = dot(rect.pos - aim.pos, n) / den; if (tt <= 0) return false;
    V3 local = rotate(conj(rect.rot), aim.pos + d * tt - rect.pos);
    float uu = local.x / w + 0.5f, vv = 0.5f - local.y / h;
    if (!clamp && (uu < 0 || uu > 1 || vv < 0 || vv > 1)) return false;
    *t = tt; *u = std::max(0.f, std::min(1.f, uu)); *v = std::max(0.f, std::min(1.f, vv));
    return true;
}

bool Interaction::panelRay(const Pose& aim, float* t, float* px, float* py, bool clamp) const {
    if (!panel.visible) return false;
    float u, v; if (!rectRay(aim, panel.pose, panel.width, panel.height, t, &u, &v, clamp)) return false;
    *px = u * panel.pxW; *py = v * panel.pxH;
    return true;
}

bool Interaction::badgeRay(const Pose& aim, float* t, float* u) const {
    float v; return panel.visible && !scene.empty() && rectRay(aim, badgePose(), kBadgeW, kBadgeH, t, u, &v, false);
}

Interaction::Target Interaction::aimAt(const Pose& aim) const {
    Target out;
    if (!scene.empty()) {
        Pose inv = inverse(model);
        V3 o = apply(inv, aim.pos), d = norm(rotate(inv.rot, forward(aim)));
        Hit f = scene.pick(o, d);
        float maxT = f.mesh >= 0 ? f.t * 1.01f + 1e-3f / model.scale : 1e30f;
        Hit e = scene.pickEdge(o, d, 0.012f, maxT);                     // edges win within ~0.7 degrees
        Hit h = e.mesh >= 0 ? e : f;
        if (h.mesh >= 0) { out.t = h.t * model.scale; out.elem = {h.mesh, e.mesh >= 0 ? 'E' : 'F', h.face}; out.point = h.point; }
    }
    float t, px, py;
    if (panelRay(aim, &t, &px, &py, false) && t < out.t) { out = {}; out.panel = true; out.t = t; out.px = px; out.py = py; }
    if (badgeRay(aim, &t, &px) && t < out.t) { out = {}; out.badge = true; out.t = t; }
    return out;
}

void Interaction::modelRay(const Pose& aim, V3* o, V3* d) const {
    Pose inv = inverse(model); *o = apply(inv, aim.pos); *d = norm(rotate(inv.rot, forward(aim)));
}

float Interaction::depthMm(int hand, const Pose& aim) const {
    if (target[hand].elem.valid()) return target[hand].t / model.scale;
    V3 o, d; modelRay(aim, &o, &d); return std::max(len(scene.center() - o), 1.f);
}

void Interaction::update(const HandInput hands[2], float dt) {
    bool trig[2], squeeze[2], host = (bool)onViewport;
    for (int i = 0; i < 2; ++i) {
        const HandInput& h = hands[i];
        trig[i] = h.active && h.select > 0.55f; squeeze[i] = h.active && h.squeeze > 0.55f;
        target[i] = h.active ? aimAt(h.aim) : Target{};
        hover[i] = (target[i].panel || host) ? ElemRef{} : target[i].elem;
        if (h.active && h.menu && !prevMenu[i] && onMenu) onMenu();
        prevMenu[i] = h.active && h.menu;
        if (h.active && h.context && !prevStick[i] && onContextMenu) onContextMenu();
        prevStick[i] = h.active && h.context;
    }
    // ---- the app's view: a pressed trigger off the panel is a finger on it, a ray pointing into the scene a hovering mouse
    if (host) {
        if (viewHand >= 0) {
            int i = viewHand; V3 o, d; modelRay(hands[i].aim, &o, &d);
            if (trig[i] && hands[i].active) onViewport(1, o, d, 0);
            else { onViewport(2, o, d, 0); viewHand = -1; }
        }
        if (viewHand < 0 && pointerHand < 0) for (int i : {1, 0})
            if (trig[i] && !prevTrig[i] && !target[i].panel && !target[i].badge && !grab[i].on) {
                V3 o, d; modelRay(hands[i].aim, &o, &d); viewHand = i; viewHover = false;
                onViewport(0, o, d, depthMm(i, hands[i].aim)); break;
            }
        int hv = -1;
        if (viewHand < 0) for (int i : {1, 0}) if (hands[i].active && !trig[i] && !target[i].panel && !target[i].badge && !grab[i].on) { hv = i; break; }
        if (hv >= 0) { V3 o, d; modelRay(hands[hv].aim, &o, &d); onViewport(3, o, d, depthMm(hv, hands[hv].aim)); viewHover = true; }
        else if (viewHover && viewHand < 0) { onViewport(4, V3{}, V3{}, 0); viewHover = false; }
    }
    // ---- the panel: one touch pointer, owned by the hand that pressed
    if (pointerHand >= 0) {
        int i = pointerHand; float t, px, py;
        if (!trig[i] || !hands[i].active) { if (onPanelPointer) onPanelPointer(PanelUp, hoverX, hoverY); pointerHand = -1; }
        else if (panelRay(hands[i].aim, &t, &px, &py, true)) { hoverX = px; hoverY = py; if (onPanelPointer) onPanelPointer(PanelMove, px, py); }
    }
    if (pointerHand < 0) {
        for (int i : {1, 0}) if (trig[i] && !prevTrig[i] && target[i].panel && !grab[i].on) {
            pointerHand = i; hoverX = target[i].px; hoverY = target[i].py;
            if (onPanelPointer) onPanelPointer(PanelDown, hoverX, hoverY);
            break;
        }
    }
    int hv = pointerHand >= 0 ? pointerHand : (target[1].panel ? 1 : target[0].panel ? 0 : -1);
    if (pointerHand < 0) {
        if (hv >= 0) {
            if (hv != hoverHand || std::fabs(target[hv].px - hoverX) + std::fabs(target[hv].py - hoverY) > 0.5f) {
                hoverX = target[hv].px; hoverY = target[hv].py; if (onPanelPointer) onPanelPointer(PanelHover, hoverX, hoverY);
            }
            if (std::fabs(hands[hv].stickY) > 0.2f && onPanelPointer) onPanelPointer(PanelScroll, hoverX, hands[hv].stickY * dt * 12.f);
        } else if (hoverHand >= 0 && onPanelPointer) onPanelPointer(PanelLeave, hoverX, hoverY);
    }
    hoverHand = hv;

    // ---- grabs: grip anywhere on the model / panel, or trigger held on the model
    for (int i = 0; i < 2; ++i) {
        const HandInput& h = hands[i];
        bool trigStart = trig[i] && !prevTrig[i], sqStart = squeeze[i] && !prevSqueeze[i];
        if (trigStart && pointerHand != i) {
            triggerT[i] = 0; triggerStart[i] = h.aim.pos;
            triggerOnModel[i] = !target[i].panel && !target[i].badge; triggerOnBadge[i] = target[i].badge;
        }
        if (trig[i]) triggerT[i] += dt;
        if (!grab[i].on) {
            if (sqStart && (target[i].panel || target[i].badge)) grab[i] = {true, unscaled(h.grip), panel.pose, 0, h.aim.pos, true};
            else if (sqStart || (!host && trigStart && pointerHand != i && target[i].elem.valid()))
                grab[i] = {true, unscaled(h.grip), model, 0, h.aim.pos, false};
        }
        bool holding = (grab[i].panel || host) ? squeeze[i] : (squeeze[i] || trig[i]);
        if (grab[i].on && !holding) grab[i].on = false;
        // a short trigger press that did not move = click: pick what the ray is on
        if (!host && !trig[i] && prevTrig[i] && triggerOnModel[i] && triggerT[i] < 0.35f && len(h.aim.pos - triggerStart[i]) < 0.02f) {
            ElemRef e = target[i].panel ? ElemRef{} : target[i].elem;
            if (onPick) onPick(e, target[i].point);
            if (!hostSelection) {
                auto it = std::find(selected.begin(), selected.end(), e);
                if (!e.valid()) selected.clear(); else if (it != selected.end()) selected.erase(it); else selected.push_back(e);
            }
            triggerOnModel[i] = false;
        }
        if (!trig[i] && prevTrig[i] && triggerOnBadge[i]) { if (target[i].badge) setTrueScale(!trueScale); triggerOnBadge[i] = false; }
        prevTrig[i] = trig[i]; prevSqueeze[i] = squeeze[i];
        // thumbstick, when not scrolling the panel: scale and turn the model about its centre
        if (h.active && !target[i].panel && !target[i].badge && (std::fabs(h.stickX) > 0.2f || std::fabs(h.stickY) > 0.2f) && !scene.empty()) {
            V3 c = apply(model, scene.center());
            float s = trueScale ? 1.f : std::exp(h.stickY * dt * 1.2f); Q turn = axisAngle({0, 1, 0}, -h.stickX * dt * 1.8f);
            model.rot = qnorm(turn * model.rot); model.scale *= s; model.pos = c + rotate(turn, (model.pos - c) * s);
        }
    }
    bool modelGrab[2] = {grab[0].on && !grab[0].panel, grab[1].on && !grab[1].panel};
    if (modelGrab[0] && modelGrab[1]) {
        V3 a = hands[0].grip.pos, b = hands[1].grip.pos, mid = (a + b) * 0.5f, dir = b - a; dir.y = 0;
        if (!twoHand) { twoHand = true; twoDist = std::max(len(b - a), 1e-3f); twoModel = model; twoMid = mid; twoDir = norm(dir); }
        float s = len(b - a) / twoDist; V3 nd = norm(dir);
        Q turn = axisAngle({0, 1, 0}, std::atan2(cross(twoDir, nd).y, dot(twoDir, nd)));
        if (trueScale) s = 1;
        else if (std::fabs(std::log(twoModel.scale * s / kTrueScale)) < 0.05f) s = kTrueScale / twoModel.scale;   // snap to 1:1
        model.rot = qnorm(turn * twoModel.rot); model.scale = twoModel.scale * s; model.pos = mid + rotate(turn, (twoModel.pos - twoMid) * s);
        triggerOnModel[0] = triggerOnModel[1] = false;                   // not a click
    } else {
        if (twoHand) {                                                 // re-anchor the remaining hand
            for (int i = 0; i < 2; ++i) if (modelGrab[i]) grab[i] = {true, unscaled(hands[i].grip), model, 1, hands[i].aim.pos, false};
            twoHand = false;
        }
        for (int i = 0; i < 2; ++i) if (grab[i].on) {
            Pose delta = compose(unscaled(hands[i].grip), inverse(grab[i].handStart));
            if (grab[i].panel) { panel.pose = compose(delta, grab[i].objStart); panel.pose.scale = 1; }
            else model = compose(delta, grab[i].objStart);
        }
    }
    badgeHot = target[0].badge || target[1].badge;
    last[0] = hands[0]; last[1] = hands[1];
}

void Interaction::render(Renderer& r, const M4& vp, V3 eye) {
    viewer = eye;
    r.upload(scene);
    Lines l; std::vector<float> dots;
    auto dot = [&](V3 p, V3 c) { dots.insert(dots.end(), {p.x, p.y, p.z, c.x, c.y, c.z}); };
    for (int x = -10; x <= 10; ++x) {                                 // 4 m floor grid, 20 cm cells
        V3 c = (x == 0) ? V3{0.32f, 0.36f, 0.42f} : V3{0.22f, 0.25f, 0.29f};
        l.add({x * 0.2f, 0, -2}, {x * 0.2f, 0, 2}, c); l.add({-2, 0, x * 0.2f}, {2, 0, x * 0.2f}, c);
    }
    for (int i = 0; i < 2; ++i) {
        const HandInput& h = last[i]; if (!h.active) continue;
        const Target& t = target[i]; bool hit = t.panel || t.elem.valid();
        float len_ = hit ? t.t : 1.5f; V3 end = h.aim.pos + forward(h.aim) * len_;
        V3 col = t.panel ? V3{0.92f, 0.94f, 0.97f} : t.elem.valid() ? V3{0.35f, 0.75f, 1.f} : V3{0.55f, 0.60f, 0.66f};
        l.add(h.aim.pos, end, col);
        if (hit) dot(end, t.panel ? (pointerHand == i ? V3{0.26f, 0.56f, 0.87f} : V3{1, 1, 1}) : V3{0.35f, 0.75f, 1.f});
        V3 g = h.grip.pos;                                             // controller: a small axis cross at the grip
        l.add(g, g + rotate(h.grip.rot, {0.03f, 0, 0}), {0.9f, 0.3f, 0.3f});
        l.add(g, g + rotate(h.grip.rot, {0, 0.03f, 0}), {0.3f, 0.9f, 0.3f});
        l.add(g, g + rotate(h.grip.rot, {0, 0, 0.05f}), {0.3f, 0.5f, 0.95f});
    }
    ElemRef hv = onViewport ? hostHover : hover[1].valid() ? hover[1] : hover[0];
    if (!scene.empty()) r.drawScene(scene, model, vp, eye, hv, selected, fade);
    r.drawLines(l, vp);
    // what the app's view draws over the model: always on top, as on a screen
    glDisable(GL_DEPTH_TEST);
    if (sketchVisible && sketchTex) {
        V3 c[4]; for (int k = 0; k < 4; ++k) c[k] = apply(model, sketchCorners[k]);
        V3 ax = c[1] - c[0], ay = c[0] - c[3]; float w = len(ax), h = len(ay);
        if (w > 1e-6f && h > 1e-6f) {
            V3 x = ax * (1 / w), y = norm(ay - x * mcxr::dot(ay, x)), z = cross(x, y);
            r.drawPanel(sketchTex, false, Pose{(c[0] + c[2]) * 0.5f, fromBasis(x, y, z), 1}, w, h, 0, vp);
        }
    }
    if (!overlay.lines.empty()) {
        Lines ol; const auto& v = overlay.lines;
        for (size_t i = 0; i + 8 < v.size(); i += 9) ol.add(apply(model, {v[i], v[i + 1], v[i + 2]}), apply(model, {v[i + 3], v[i + 4], v[i + 5]}), {v[i + 6], v[i + 7], v[i + 8]});
        r.drawLines(ol, vp);
    }
    {   // dots, by size
        const auto& v = overlay.dots; std::map<int, std::vector<float>> bySize;
        for (size_t i = 0; i + 6 < v.size(); i += 7) { V3 p = apply(model, {v[i], v[i + 1], v[i + 2]}); bySize[(int)v[i + 6]].insert(bySize[(int)v[i + 6]].end(), {p.x, p.y, p.z, v[i + 3], v[i + 4], v[i + 5]}); }
        for (auto& [sz, pts] : bySize) r.drawDots(pts, sz * 1.5f, vp);
    }
    glDisable(GL_DEPTH_TEST);                                           // (drawDots turns it back on)
    if (labelTex.size() > 256) { for (auto& [k, t] : labelTex) glDeleteTextures(1, &t.first); labelTex.clear(); }
    for (auto& [p0, text] : overlay.labels) {                         // text labels: billboards, about 1 degree tall
        auto it = labelTex.find(text);
        if (it == labelTex.end()) {
            std::string t = text.substr(0, 48); int W = Canvas::textWidth(t, 2) + 24, H = 44;
            Canvas cv(W, H); cv.rect(0, 0, W, H, 34, 39, 45, 10); cv.text(12, 6, t, 2, 235, 238, 242);
            it = labelTex.emplace(text, std::make_pair(r.makeTexture(W, H, cv.px.data()), std::make_pair(W, H))).first;
        }
        V3 p = apply(model, p0); float dist = std::max(len(eye - p), 0.05f);
        V3 z = norm(eye - p), x = norm(cross(V3{0, 1, 0}, z)); if (len(x) < 0.5f) x = {1, 0, 0}; V3 y = cross(z, x);
        float h = 0.02f * dist, w = h * it->second.second.first / it->second.second.second;
        r.drawPanel(it->second.first, false, Pose{p + x * (w * 0.5f + 0.012f * dist) + y * (h * 0.7f), fromBasis(x, y, z), 1}, w, h, h * 0.25f, vp);
    }
    glEnable(GL_DEPTH_TEST);
    if (panel.visible && panel.tex) {
        // a dark back plate with a slim border, then the UI itself
        static GLuint plate = 0;
        if (!plate) { const unsigned char px[4] = {14, 17, 21, 255}; plate = r.makeTexture(1, 1, px); }
        Pose back = panel.pose; back.pos = back.pos - rotate(back.rot, {0, 0, 0.004f});
        r.drawPanel(plate, false, back, panel.width + 0.02f, panel.height + 0.02f, 0.03f, vp);
        r.drawPanel(panel.tex, panel.external, panel.pose, panel.width, panel.height, 0.022f, vp);
    }
    if (panel.visible && !scene.empty()) {
        // the scale badge: "SCALE 1:4.2   [TRUE SIZE]", or "SCALE 1:1 TRUE SIZE   [FIT VIEW]"
        std::string ratio = scaleText(), button = trueScale ? "FIT VIEW" : "TRUE SIZE";
        std::string key = ratio + "|" + button + (badgeHot ? "*" : "");
        if (key != badgeDrawn || !badgeTex || !glIsTexture(badgeTex)) {
            int W = (int)(kBadgeW * kBadgePx), H = (int)(kBadgeH * kBadgePx);
            Canvas c(W, H); c.rect(0, 0, W, H, 14, 17, 21);
            c.text(32, (H - 48) / 2, "SCALE", 3, 138, 150, 163);
            if (trueScale) c.text(180, (H - 64) / 2, "1:1", 4, 110, 200, 140), c.text(180 + 4 * 32, (H - 48) / 2, "TRUE SIZE", 3, 110, 200, 140);
            else c.text(180, (H - 64) / 2, ratio, 4, 235, 238, 242);
            int bw = 280, x0 = W - 16 - bw;
            uint8_t g = badgeHot ? 125 : 111, rr = badgeHot ? 58 : 42, bb = badgeHot ? 221 : 201;
            c.rect(x0, 14, W - 16, H - 14, rr, g, bb, (H - 28) / 2);
            c.text(x0 + (bw - Canvas::textWidth(button, 3)) / 2, (H - 48) / 2, button, 3, 255, 255, 255);
            badgeTex = r.makeTexture(W, H, c.px.data(), glIsTexture(badgeTex) ? badgeTex : 0); badgeDrawn = key;
        }
        r.drawPanel(badgeTex, false, badgePose(), kBadgeW, kBadgeH, kBadgeH / 2, vp);
    }
    if (cardTex && last[0].active) {                                   // info card floats above the left hand, facing it
        Pose p = unscaled(last[0].grip);
        p.pos = p.pos + rotate(p.rot, {0, 0.09f, -0.02f}); p.rot = qnorm(p.rot * axisAngle({1, 0, 0}, -0.6f));
        float w = 0.16f; r.drawPanel(cardTex, false, p, w, w * cardH / std::max(cardW, 1), 0.01f, vp);
    }
    r.drawDots(dots, 14.f, vp);
}

}  // namespace mcxr
