// What the user does in XR, independent of OpenXR: hands in, model pose / highlights / callbacks out. The OpenXR session
// (xr_session) and the headset-free preview both feed it HandInput every frame.
//
//   aim + trigger click on a face ........ select it (FreeCAD Face<i> of that object)
//   trigger / grip held on the model ..... grab: the model follows the hand (move + rotate)
//   both hands holding ................... scale and turn the model between the hands
//   thumbstick (if any) .................. up/down scales, left/right turns the model
//   menu / A / X button .................. next document
#pragma once
#include <functional>
#include <string>
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
};

class Interaction {
public:
    Scene scene;
    Pose model{};               // scene (mm) -> world (m)
    FaceRef hover[2], selected;
    std::function<void(const std::string& object, int face)> onSelect;   // face 0 = cleared
    std::function<void()> onNextDocument;
    GLuint panelTex = 0; int panelW = 0, panelH = 0;                     // info panel next to the left hand

    /** Puts the model ~0.7 m in front of the viewer, a little below eye height, about 40 cm across. */
    void placeModel(const Pose& head);
    void update(const HandInput hands[2], float dt);
    void render(Renderer& r, const M4& viewProj, V3 eye);

private:
    struct Grab { bool on = false; Pose handStart, modelStart; float t = 0; V3 aimStart; };
    Grab grab[2];
    bool twoHand = false; float twoDist = 0; Pose twoModel; V3 twoMid, twoDir;
    bool prevPress[2] = {false, false}, prevMenu[2] = {false, false};
    HandInput last[2];
    float scaleFit = 0.001f;
    FaceRef pickRay(const Pose& aim, float* dist = nullptr) const;
    static bool pressed(const HandInput& h) { return h.select > 0.55f || h.squeeze > 0.55f; }
};

}  // namespace mcxr
