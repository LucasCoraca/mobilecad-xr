// What the user does in XR, independent of OpenXR: hands in, model / panel poses, highlights and callbacks out. The OpenXR
// session (xr_session) and the headset-free preview both feed it HandInput every frame.
//
//   ray on the UI panel:  trigger = touch (tap, drag)   grip = move the panel   thumbstick up/down = scroll
//   ray on the model:     trigger click = pick the face / edge (into the document's selection)
//                         grip, or trigger held = grab: the model follows the hand (move + rotate)
//   both hands grabbing:  scale and turn the model between the hands
//   thumbstick (not on the panel): up/down scales, left/right turns the model
//   menu / A / X:         onMenu (the app shows / recentres the panel, or switches documents)
//
// With `onViewport` set (MobileCAD XR) the space around the panel is the app's 3D view: the trigger anywhere off the panel
// is a finger on that view (tap, drag, double tap) and pointing is a hovering mouse, so every tool of the app (picking,
// the sketcher, Draft points, drag arrows) works on the model in the room; the grip moves the model,
// B / Y opens the view's context menu. The app sends back what its view draws: preselection, overlay, the sketch.
//   scale badge (under the panel): shows the model's scale; its button switches true size (1 mm = 1 mm) on / off
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "renderer.h"
#include "scene.h"
#include "xmath.h"

namespace mcxr {

struct HandInput {
    bool active = false;        // pose tracked this frame
    Pose aim, grip;             // world (stage) space, scale 1; aim points down -Z
    float select = 0;           // trigger value 0..1
    float squeeze = 0;          // grip value 0..1 (0 on controllers without one)
    float stickX = 0, stickY = 0;
    bool menu = false;          // menu / A / X button
    bool context = false;       // B / Y button: the view's context menu
};

/** A flat UI surface in the world (the app's interface). Local +Z faces the user, X right, Y up; size in metres. */
struct Panel {
    bool visible = false;
    Pose pose;                  // centre
    float width = 1.0f, height = 0.625f;
    int pxW = 1600, pxH = 1000; // texture / touch coordinate space
    GLuint tex = 0; bool external = false;
};

enum PanelAction { PanelDown = 0, PanelMove = 1, PanelUp = 2, PanelHover = 3, PanelLeave = 4, PanelScroll = 5 };

class Interaction {
public:
    Scene scene;
    Pose model{};               // scene (mm) -> world (m)
    Panel panel;
    ElemRef hover[2];
    /** The document's selection. With `hostSelection` the app owns it (onPick reports clicks and the app sends the
     *  selection back); otherwise clicks toggle it here. */
    std::vector<ElemRef> selected; bool hostSelection = false;
    std::function<void(const ElemRef& ref, V3 modelPoint)> onPick;          // invalid ref = click on empty space
    std::function<void(int action, float x, float y)> onPanelPointer;      // x, y in panel pixels (scroll: y = amount)
    std::function<void()> onMenu;
    /** True size: the model at its real size, 1 mm = 1 mm. Scaling gestures then only move and turn it (free scaling
     *  outside this mode snaps to 1:1 on the way through). Toggled by the scale badge under the panel. */
    bool trueScale = false;
    void setTrueScale(bool on);
    float floorY = 0;            // world height of the floor (0 in stage space)
    /** "1:1", "1:4.2", "3:1": world size : real size. */
    std::string scaleText() const;
    /** Centre of the scale badge's button (world), for scripted tests. */
    V3 badgeButton() const;
    // ---- the app's 3D view (see the top)
    std::function<void(int action, V3 o, V3 d, float depthMm)> onViewport;   // 0 press 1 drag 2 release 3 hover 4 leave; model mm
    std::function<void()> onContextMenu;
    ElemRef hostHover;                       // the app's preselection
    float fade = 1;                          // < 1: the model drawn see-through (the app dims it while a sketch is edited)
    /** What the app's view draws over the model, in model coordinates (mm): lines (x y z x y z r g b), dots (x y z r g b
     *  sizeDp), text labels. */
    struct Overlay { std::vector<float> lines, dots; std::vector<std::pair<V3, std::string>> labels; } overlay;
    /** The sketch being edited, drawn by the app into a texture lying on the sketch plane: corners TL TR BR BL (model mm). */
    GLuint sketchTex = 0; bool sketchVisible = false; V3 sketchCorners[4];
    // optional info card above the left hand
    GLuint cardTex = 0; int cardW = 0, cardH = 0;

    /** Puts the model ~0.7 m in front of the viewer, a little below eye height, about 40 cm across. */
    void placeModel(const Pose& head);
    /** Puts the UI panel to the left-front of the viewer, turned towards them. */
    void placePanel(const Pose& head);
    void update(const HandInput hands[2], float dt);
    void render(Renderer& r, const M4& viewProj, V3 eye);

private:
    struct Grab { bool on = false; Pose handStart, objStart; float t = 0; V3 aimStart; bool panel = false; };
    Grab grab[2];
    bool twoHand = false; float twoDist = 0; Pose twoModel; V3 twoMid, twoDir;
    bool prevTrig[2] = {false, false}, prevSqueeze[2] = {false, false}, prevMenu[2] = {false, false};
    int pointerHand = -1;                     // hand holding a touch on the panel
    int hoverHand = -1; float hoverX = 0, hoverY = 0;
    float triggerT[2] = {0, 0}; V3 triggerStart[2]; bool triggerOnModel[2] = {false, false};
    Pose fitPose; bool fitSaved = false;      // the pose to return to when true size is switched off
    V3 viewer{0, 1.6f, 0};
    bool triggerOnBadge[2] = {false, false}, badgeHot = false;
    int viewHand = -1; bool viewHover = false; bool prevStick[2] = {false, false};
    std::map<std::string, std::pair<GLuint, std::pair<int, int>>> labelTex;
    void modelRay(const Pose& aim, V3* o, V3* d) const;
    float depthMm(int hand, const Pose& aim) const;
    GLuint badgeTex = 0; std::string badgeDrawn;
    Pose badgePose() const;
    float lowestY() const;
    struct Target { bool panel = false, badge = false; float t = 1e30f; float px = 0, py = 0; ElemRef elem; V3 point; };
    Target target[2];
    HandInput last[2];
    Target aimAt(const Pose& aim) const;
    bool panelRay(const Pose& aim, float* t, float* px, float* py, bool clamp) const;
    bool badgeRay(const Pose& aim, float* t, float* u) const;
};

}  // namespace mcxr
