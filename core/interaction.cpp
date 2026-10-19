#include "interaction.h"

namespace mcxr {

static V3 forward(const Pose& p) { return rotate(p.rot, V3{0, 0, -1}); }
static Pose unscaled(const Pose& p) { return Pose{p.pos, p.rot, 1}; }

void Interaction::placeModel(const Pose& head) {
    if (scene.empty()) return;
    float r = std::max(scene.radius(), 1e-3f);
    scaleFit = 0.2f / r;                                              // ~40 cm across
    V3 f = forward(head); f.y = 0; f = len(f) > 1e-3f ? norm(f) : V3{0, 0, -1};
    V3 target = head.pos + f * 0.7f + V3{0, -0.15f, 0};
    float yaw = std::atan2(-f.x, -f.z);
    // FreeCAD is Z-up, OpenXR is Y-up: turn the model upright, then face the viewer
    Q upright = axisAngle({1, 0, 0}, -1.5707963f);
    Q rot = qnorm(axisAngle({0, 1, 0}, yaw) * upright);
    model = {V3{}, rot, scaleFit};
    model.pos = target - apply(model, scene.center());
    selected = {}; hover[0] = hover[1] = {};
}

FaceRef Interaction::pickRay(const Pose& aim, float* dist) const {
    if (scene.empty()) return {};
    Pose inv = inverse(model);
    V3 o = apply(inv, aim.pos); V3 d = norm(rotate(inv.rot, forward(aim)));
    Hit h = scene.pick(o, d);
    if (h.mesh < 0) return {};
    if (dist) *dist = h.t * model.scale;
    return {h.mesh, h.face};
}

void Interaction::update(const HandInput hands[2], float dt) {
    bool down[2];
    for (int i = 0; i < 2; ++i) {
        const HandInput& h = hands[i]; down[i] = h.active && pressed(h);
        hover[i] = h.active ? pickRay(h.aim) : FaceRef{};
        // press: start a grab (the model follows the hand); a short press without movement is a click = select
        if (down[i] && !prevPress[i]) { grab[i] = {true, unscaled(h.grip), model, 0, h.aim.pos}; }
        if (down[i]) grab[i].t += dt;
        if (!down[i] && prevPress[i] && grab[i].on) {
            bool click = grab[i].t < 0.35f && len(h.aim.pos - grab[i].aimStart) < 0.02f;
            grab[i].on = false;
            if (click) {
                FaceRef f = hover[i];
                selected = (f == selected) ? FaceRef{} : f;
                if (onSelect) onSelect(selected.mesh >= 0 ? scene.meshes[selected.mesh].name : std::string(), selected.face);
            }
        }
        if (h.active && h.menu && !prevMenu[i] && onNextDocument) onNextDocument();
        prevMenu[i] = h.active && h.menu; prevPress[i] = down[i];
        // thumbstick: scale and turn about the model's centre
        if (h.active && (std::fabs(h.stickX) > 0.2f || std::fabs(h.stickY) > 0.2f) && !scene.empty()) {
            V3 c = apply(model, scene.center());
            float s = std::exp(h.stickY * dt * 1.2f); Q turn = axisAngle({0, 1, 0}, -h.stickX * dt * 1.8f);
            model.rot = qnorm(turn * model.rot); model.scale *= s;
            model.pos = c + rotate(turn, (model.pos - c) * s);
        }
    }
    if (down[0] && down[1] && hands[0].active && hands[1].active) {
        V3 a = hands[0].grip.pos, b = hands[1].grip.pos, mid = (a + b) * 0.5f, dir = b - a; dir.y = 0;
        if (!twoHand) { twoHand = true; twoDist = std::max(len(b - a), 1e-3f); twoModel = model; twoMid = mid; twoDir = norm(dir); }
        float s = len(b - a) / twoDist; V3 nd = norm(dir);
        float ang = std::atan2(cross(twoDir, nd).y, dot(twoDir, nd));
        Q turn = axisAngle({0, 1, 0}, ang);
        // scale + turn about the starting midpoint, then follow the midpoint
        model.rot = qnorm(turn * twoModel.rot); model.scale = twoModel.scale * s;
        model.pos = mid + rotate(turn, (twoModel.pos - twoMid) * s);
        grab[0].t = grab[1].t = 1;                                    // not a click
    } else {
        if (twoHand) {                                                // re-anchor the remaining hand
            for (int i = 0; i < 2; ++i) if (down[i]) grab[i] = {true, unscaled(hands[i].grip), model, 1, hands[i].aim.pos};
            twoHand = false;
        }
        for (int i = 0; i < 2; ++i) if (down[i] && grab[i].on) {
            Pose delta = compose(unscaled(hands[i].grip), inverse(grab[i].handStart));
            model = compose(delta, grab[i].modelStart);
        }
    }
    last[0] = hands[0]; last[1] = hands[1];
}

void Interaction::render(Renderer& r, const M4& vp, V3 eye) {
    r.upload(scene);
    Lines l;
    for (int x = -10; x <= 10; ++x) {                                 // 4 m floor grid, 20 cm cells
        V3 c = (x == 0) ? V3{0.32f, 0.36f, 0.42f} : V3{0.22f, 0.25f, 0.29f};
        l.add({x * 0.2f, 0, -2}, {x * 0.2f, 0, 2}, c); l.add({-2, 0, x * 0.2f}, {2, 0, x * 0.2f}, c);
    }
    for (int i = 0; i < 2; ++i) {
        const HandInput& h = last[i]; if (!h.active) continue;
        float d = 1.5f; FaceRef f = pickRay(h.aim, &d);
        V3 col = f.mesh >= 0 ? V3{0.35f, 0.75f, 1.f} : V3{0.75f, 0.78f, 0.82f};
        l.add(h.aim.pos, h.aim.pos + forward(h.aim) * (f.mesh >= 0 ? d : 1.5f), col);
        // controller: a small axis cross at the grip
        V3 g = h.grip.pos;
        l.add(g, g + rotate(h.grip.rot, {0.03f, 0, 0}), {0.9f, 0.3f, 0.3f});
        l.add(g, g + rotate(h.grip.rot, {0, 0.03f, 0}), {0.3f, 0.9f, 0.3f});
        l.add(g, g + rotate(h.grip.rot, {0, 0, 0.05f}), {0.3f, 0.5f, 0.95f});
    }
    if (!scene.empty()) r.drawScene(scene, model, vp, eye, hover[0].mesh >= 0 ? hover[0] : hover[1], selected);
    r.drawLines(l, vp);
    if (panelTex && last[0].active) {                                 // info panel floats above the left hand, facing it
        Pose p = unscaled(last[0].grip);
        p.pos = p.pos + rotate(p.rot, {0, 0.09f, -0.02f});
        p.rot = qnorm(p.rot * axisAngle({1, 0, 0}, -0.6f));
        float w = 0.16f; r.drawPanel(panelTex, p, w, w * panelH / std::max(panelW, 1), vp);
    }
}

}  // namespace mcxr
