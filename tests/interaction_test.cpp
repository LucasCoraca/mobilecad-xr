// Headless checks of core/interaction (no headset, no GL context: update() only): the steadied ray, the marking menu and
// the haptic ticks. Build and run with tests/run.sh (in the mobilecad-xrsim image, which has the GL / OpenXR headers).
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include "interaction.h"

using namespace mcxr;

static int failures = 0;
static void check(bool ok, const char* what, double value = NAN) {
    if (std::isnan(value)) std::printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
    else std::printf("%-58s %s  (%.4g)\n", what, ok ? "PASS" : "FAIL", value);
    if (!ok) ++failures;
}
static float angleDeg(Q a, Q b) { Q d = a * conj(b); return 2 * std::acos(std::min(1.f, std::fabs(d.w))) * 57.29578f; }
static V3 fwd(const Pose& p) { return rotate(p.rot, V3{0, 0, -1}); }

struct Rig {
    Interaction ia; HandInput h[2]; const float dt = 1.f / 72;
    std::vector<std::string> items{"Sketch", "Extrude (Pad)", "Plane", "Hide / show", "Delete 1", "Clear selection", "Zoom to fit", "Isometric"};
    int opened = 0, chosen = -1, context = 0;
    Rig() {
        ia.plate = false; ia.headPose = Pose{V3{0, 1.6f, 0}, Q{}, 1};
        ia.onPieOpen = [this] { ++opened; ia.setPieItems(items, std::vector<bool>(items.size(), false)); };
        ia.onPieSelect = [this](int i) { chosen = i; };
        ia.onContextMenu = [this] { ++context; };
        h[1].active = true; h[1].aim = h[1].grip = Pose{V3{0.2f, 1.3f, -0.4f}, Q{}, 1};
    }
    void step(int n = 1) { for (int k = 0; k < n; ++k) { ia.update(h, dt); } }
};

int main() {
    {   // smoothDamp: reaches the target, never past it
        float v = 0, x = 0; bool over = false;
        for (int k = 0; k < 72; ++k) { x = smoothDamp(x, 1.f, v, 0.1f, 1.f / 72); over |= x > 1.f; }
        check(!over && std::fabs(x - 1) < 1e-3f, "smoothDamp settles on its target without overshoot", x);
    }
    {   // a still controller with tremor: the ray shakes less than the controller
        std::normal_distribution<float> n(0.f, 0.25f / 57.3f); Q base{};
        // the steadied ray read back as its cursor on a panel 1 m ahead, against where the raw ray meets it
        Rig p; p.ia.panel.visible = true; p.ia.panel.pose = Pose{V3{0.2f, 1.3f, -1.4f}, Q{}, 1}; p.ia.panel.width = 1; p.ia.panel.height = 0.6f;
        std::mt19937 rng2(7); double hitSum = 0, rawHit = 0; V3 prevHit{}, prevRawHit{};
        for (int k = 0; k < 360; ++k) {
            p.h[1].aim.rot = qnorm(base * axisAngle({1, 0, 0}, n(rng2)) * axisAngle({0, 1, 0}, n(rng2)));
            p.step();
            auto c = p.ia.uiCursors(); if (c.empty()) continue;
            V3 rawHit3 = p.h[1].aim.pos + fwd(p.h[1].aim) * (1.0f / std::max(1e-3f, -fwd(p.h[1].aim).z));
            if (k > 36) { hitSum += len(c[0] - prevHit); rawHit += len(rawHit3 - prevRawHit); }
            prevHit = c[0]; prevRawHit = rawHit3;
        }
        check(hitSum < rawHit * 0.6, "tremor: the cursor moves less than the raw ray (ratio)", hitSum / rawHit);
    }
    {   // a fast swing is followed at once
        Rig p; p.ia.panel.visible = true; p.ia.panel.pose = Pose{V3{0.2f, 1.3f, -1.4f}, Q{}, 1}; p.ia.panel.width = 3; p.ia.panel.height = 2;
        float lag = 0;
        for (int k = 0; k < 20; ++k) {
            p.h[1].aim.rot = axisAngle({0, 1, 0}, -3.f * k * p.dt);       // 3 rad/s to the right
            p.step();
            auto c = p.ia.uiCursors(); if (c.empty()) continue;
            V3 d = fwd(p.h[1].aim); V3 raw = p.h[1].aim.pos + d * (1.0f / -d.z);
            lag = len(c[0] - raw);
        }
        check(lag < 0.01f, "swing at 3 rad/s: the cursor stays on the raw ray (m)", lag);
    }
    {   // the trigger on its way down holds the ray
        Rig p; p.ia.panel.visible = true; p.ia.panel.pose = Pose{V3{0.2f, 1.3f, -1.4f}, Q{}, 1}; p.ia.panel.width = 1; p.ia.panel.height = 0.6f;
        p.step(10); V3 before = p.ia.uiCursors().at(0);
        p.h[1].select = 0.4f; p.h[1].aim.rot = axisAngle({1, 0, 0}, 1.5f / 57.3f);   // the press tips the controller 1.5 degrees
        p.step(4);
        float moved = len(p.ia.uiCursors().at(0) - before), full = 1.0f * std::tan(1.5f / 57.3f);
        check(moved < full * 0.3f, "half-pulled trigger: the cursor keeps most of its place (of full)", moved / full);
    }
    {   // marking menu: hold B, move right past 8 cm -> the item on the right (8 items: index 2), with ticks
        Rig r; r.step(2);
        r.h[1].context = true; r.step();
        check(r.opened == 1, "B down asks the app for the items");
        bool ticked = false; int pointed = -1;
        for (int k = 1; k <= 12; ++k) {
            r.h[1].aim.pos = V3{0.2f + 0.0075f * k, 1.3f, -0.4f}; r.step();
            ticked |= r.ia.haptic[1] > 0.2f; r.ia.haptic[1] = 0;
            if (k == 7) pointed = r.chosen;
        }
        check(pointed == -1, "between 4 and 8 cm: pointed at, not chosen yet");
        check(r.chosen == 2, "past 8 cm to the right: item 2 is chosen", r.chosen);
        check(ticked, "a haptic tick when an item is pointed at / chosen");
        r.h[1].context = false; r.step();
        check(r.context == 0, "letting go after choosing opens nothing else");
    }
    {   // let go while pointing (5 cm up): the top item
        Rig r; r.step(2); r.h[1].context = true; r.step();
        r.h[1].aim.pos = V3{0.2f, 1.35f, -0.4f}; r.step(3);
        r.h[1].context = false; r.step();
        check(r.chosen == 0, "release at 5 cm up chooses the top item", r.chosen);
    }
    {   // a short press without moving: the plain context menu
        Rig r; r.step(2); r.h[1].context = true; r.step(5); r.h[1].context = false; r.step();
        check(r.context == 1 && r.chosen == -1, "a tap on B is the plain context menu");
    }
    {   // held, then let go in the middle: nothing
        Rig r; r.step(2); r.h[1].context = true; r.step(60); r.h[1].context = false; r.step();
        check(r.context == 0 && r.chosen == -1, "held and released without moving: closed, nothing chosen");
    }
    {   // down-left diagonal with 8 items: 225 degrees -> index 5
        Rig r; r.step(2); r.h[1].context = true; r.step();
        r.h[1].aim.pos = V3{0.2f - 0.065f, 1.3f - 0.065f, -0.4f}; r.step();
        check(r.chosen == 5, "a flick down-left chooses item 5 (225 degrees)", r.chosen);
    }
    // ---- the same with tracked hands (synthetic joints through handGestures, as the headset's hand trackers give them):
    // thumb + middle pinch is B / Y, so the marking menu is a pinch and a flick
    auto handAt = [](const Pose& palm, V3 off, float middle, float pinch = 0) {
        HandInput h; syntheticHand(h, Pose{palm.pos + off, palm.rot, 1}, false, pinch, 0, middle, 0); h.active = true; h.hand = true; return h;
    };
    const Pose head{V3{0, 1.6f, 0}, Q{}, 1}; const Pose palm = palmForRay(head, false, norm(V3{0.1f, -0.35f, -1}));
    {   // pinch (thumb + middle), move the hand 10 cm right (the ring faces you: across your line of sight it is a bit less)
        Rig r; r.h[1] = handAt(palm, {}, 0); r.step(4);
        r.h[1] = handAt(palm, {}, 1); r.step(3);
        check(r.opened == 1, "hand: thumb + middle pinch opens the marking menu");
        for (int k = 1; k <= 14; ++k) { r.h[1] = handAt(palm, V3{0.0075f * k, 0, 0}, 1); r.step(); }
        check(r.chosen == 2, "hand: pinch + flick 10 cm right chooses item 2", r.chosen);
        r.h[1] = handAt(palm, V3{0.105f, 0, 0}, 0); r.step(3);
        check(r.context == 0, "hand: letting go after choosing opens nothing else");
    }
    {   // pinch, move 5 cm down, let go: the bottom item (8 items: index 4)
        Rig r; r.h[1] = handAt(palm, {}, 0); r.step(4); r.h[1] = handAt(palm, {}, 1); r.step(3);
        for (int k = 1; k <= 5; ++k) { r.h[1] = handAt(palm, V3{0, -0.01f * k, 0}, 1); r.step(); }
        r.h[1] = handAt(palm, V3{0, -0.05f, 0}, 0); r.step(3);
        check(r.chosen == 4, "hand: pinch, 5 cm down, release chooses the bottom item", r.chosen);
    }
    {   // a quick thumb + middle pinch without moving: the plain context menu
        Rig r; r.h[1] = handAt(palm, {}, 0); r.step(4); r.h[1] = handAt(palm, {}, 1); r.step(6);
        r.h[1] = handAt(palm, {}, 0); r.step(3);
        check(r.context == 1 && r.chosen == -1, "hand: a quick middle pinch is the plain context menu");
    }
    {   // an index pinch (the trigger) never opens the marking menu
        Rig r; r.h[1] = handAt(palm, {}, 0); r.step(4); r.h[1] = handAt(palm, {}, 0, 1); r.step(10);
        r.h[1] = handAt(palm, V3{0.09f, 0, 0}, 0, 1); r.step(3);
        check(r.opened == 0 && r.chosen == -1, "hand: an index pinch does not open the marking menu");
    }
    {   // a hand held out with tremor (1 mm, 0.3 degrees): the steadied cursor shakes less than without steadying
        auto run = [&](float smoothing) {
            Rig p; p.ia.aimSmoothing = smoothing; p.ia.panel.visible = true; p.ia.panel.width = 1.5f; p.ia.panel.height = 1;
            p.ia.panel.pose = Pose{palm.pos + rotate(palm.rot, V3{0, 0, -1.f}), palm.rot, 1};
            std::mt19937 rng(11); std::normal_distribution<float> mm(0.f, 0.001f), deg(0.f, 0.3f / 57.3f);
            double path = 0; V3 prev{}; bool have = false;
            for (int k = 0; k < 300; ++k) {
                Pose j{palm.pos + V3{mm(rng), mm(rng), mm(rng)}, qnorm(palm.rot * axisAngle({1, 0, 0}, deg(rng)) * axisAngle({0, 1, 0}, deg(rng))), 1};
                p.h[1] = handAt(j, {}, 0); p.step();
                auto c = p.ia.uiCursors(); if (c.empty()) { have = false; continue; }
                if (have && k > 30) path += len(c[0] - prev);
                prev = c[0]; have = true;
            }
            return path;
        };
        double raw = run(0), steadied = run(0.045f);
        check(raw > 0 && steadied < raw * 0.7, "hand tremor: the steadied cursor travels less (ratio)", steadied / raw);
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
