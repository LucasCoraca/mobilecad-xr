#include "interaction.h"
#include <algorithm>

namespace mcxr {

static V3 forward(const Pose& p) { return rotate(p.rot, V3{0, 0, -1}); }
static Pose unscaled(const Pose& p) { return Pose{p.pos, p.rot, 1}; }
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
}

void Interaction::placePanel(const Pose& head) {
    V3 f = forward(head); f.y = 0; f = len(f) > 1e-3f ? norm(f) : V3{0, 0, -1};
    V3 right = cross(f, V3{0, 1, 0});
    V3 c = head.pos + f * 0.95f - right * 0.42f + V3{0, -0.12f, 0};
    panel.pose = {c, facing(head.pos, c), 1};
    // tilt back a little, like a drafting table
    panel.pose.rot = qnorm(panel.pose.rot * axisAngle({1, 0, 0}, -0.12f));
}

bool Interaction::panelRay(const Pose& aim, float* t, float* px, float* py, bool clamp) const {
    if (!panel.visible) return false;
    V3 n = rotate(panel.pose.rot, {0, 0, 1}), d = forward(aim);
    float den = dot(d, n); if (std::fabs(den) < 1e-5f) return false;
    float tt = dot(panel.pose.pos - aim.pos, n) / den; if (tt <= 0) return false;
    V3 local = rotate(conj(panel.pose.rot), aim.pos + d * tt - panel.pose.pos);
    float u = local.x / panel.width + 0.5f, v = 0.5f - local.y / panel.height;
    if (!clamp && (u < 0 || u > 1 || v < 0 || v > 1)) return false;
    u = std::max(0.f, std::min(1.f, u)); v = std::max(0.f, std::min(1.f, v));
    *t = tt; *px = u * panel.pxW; *py = v * panel.pxH;
    return true;
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
    return out;
}

void Interaction::update(const HandInput hands[2], float dt) {
    bool trig[2], squeeze[2];
    for (int i = 0; i < 2; ++i) {
        const HandInput& h = hands[i];
        trig[i] = h.active && h.select > 0.55f; squeeze[i] = h.active && h.squeeze > 0.55f;
        target[i] = h.active ? aimAt(h.aim) : Target{};
        hover[i] = target[i].panel ? ElemRef{} : target[i].elem;
        if (h.active && h.menu && !prevMenu[i] && onMenu) onMenu();
        prevMenu[i] = h.active && h.menu;
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
        if (trigStart && pointerHand != i) { triggerT[i] = 0; triggerStart[i] = h.aim.pos; triggerOnModel[i] = !target[i].panel; }
        if (trig[i]) triggerT[i] += dt;
        if (!grab[i].on) {
            if (sqStart && target[i].panel) grab[i] = {true, unscaled(h.grip), panel.pose, 0, h.aim.pos, true};
            else if (sqStart || (trigStart && pointerHand != i && target[i].elem.valid()))
                grab[i] = {true, unscaled(h.grip), model, 0, h.aim.pos, false};
        }
        bool holding = grab[i].panel ? squeeze[i] : (squeeze[i] || trig[i]);
        if (grab[i].on && !holding) grab[i].on = false;
        // a short trigger press that did not move = click: pick what the ray is on
        if (!trig[i] && prevTrig[i] && triggerOnModel[i] && triggerT[i] < 0.35f && len(h.aim.pos - triggerStart[i]) < 0.02f) {
            ElemRef e = target[i].panel ? ElemRef{} : target[i].elem;
            if (onPick) onPick(e, target[i].point);
            if (!hostSelection) {
                auto it = std::find(selected.begin(), selected.end(), e);
                if (!e.valid()) selected.clear(); else if (it != selected.end()) selected.erase(it); else selected.push_back(e);
            }
            triggerOnModel[i] = false;
        }
        prevTrig[i] = trig[i]; prevSqueeze[i] = squeeze[i];
        // thumbstick, when not scrolling the panel: scale and turn the model about its centre
        if (h.active && !target[i].panel && (std::fabs(h.stickX) > 0.2f || std::fabs(h.stickY) > 0.2f) && !scene.empty()) {
            V3 c = apply(model, scene.center());
            float s = std::exp(h.stickY * dt * 1.2f); Q turn = axisAngle({0, 1, 0}, -h.stickX * dt * 1.8f);
            model.rot = qnorm(turn * model.rot); model.scale *= s; model.pos = c + rotate(turn, (model.pos - c) * s);
        }
    }
    bool modelGrab[2] = {grab[0].on && !grab[0].panel, grab[1].on && !grab[1].panel};
    if (modelGrab[0] && modelGrab[1]) {
        V3 a = hands[0].grip.pos, b = hands[1].grip.pos, mid = (a + b) * 0.5f, dir = b - a; dir.y = 0;
        if (!twoHand) { twoHand = true; twoDist = std::max(len(b - a), 1e-3f); twoModel = model; twoMid = mid; twoDir = norm(dir); }
        float s = len(b - a) / twoDist; V3 nd = norm(dir);
        Q turn = axisAngle({0, 1, 0}, std::atan2(cross(twoDir, nd).y, dot(twoDir, nd)));
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
    last[0] = hands[0]; last[1] = hands[1];
}

void Interaction::render(Renderer& r, const M4& vp, V3 eye) {
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
    ElemRef hv = hover[1].valid() ? hover[1] : hover[0];
    if (!scene.empty()) r.drawScene(scene, model, vp, eye, hv, selected);
    r.drawLines(l, vp);
    if (panel.visible && panel.tex) {
        // a dark back plate with a slim border, then the UI itself
        static GLuint plate = 0;
        if (!plate) { const unsigned char px[4] = {14, 17, 21, 255}; plate = r.makeTexture(1, 1, px); }
        Pose back = panel.pose; back.pos = back.pos - rotate(back.rot, {0, 0, 0.004f});
        r.drawPanel(plate, false, back, panel.width + 0.02f, panel.height + 0.02f, 0.03f, vp);
        r.drawPanel(panel.tex, panel.external, panel.pose, panel.width, panel.height, 0.022f, vp);
    }
    if (cardTex && last[0].active) {                                   // info card floats above the left hand, facing it
        Pose p = unscaled(last[0].grip);
        p.pos = p.pos + rotate(p.rot, {0, 0.09f, -0.02f}); p.rot = qnorm(p.rot * axisAngle({1, 0, 0}, -0.6f));
        float w = 0.16f; r.drawPanel(cardTex, false, p, w, w * cardH / std::max(cardW, 1), 0.01f, vp);
    }
    r.drawDots(dots, 14.f, vp);
}

}  // namespace mcxr
