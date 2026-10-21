# MobileCAD XR

Parametric CAD in the headset, on **Meta Quest 3** (and other Android OpenXR headsets): a variation of
[MobileCAD](https://github.com/LucasCoraca/mobilecad) that puts the **whole MobileCAD app** in front of you, with the
part you are building in 3D in the room and the interface arranged around you. It comes with a **headset-free test bench**, so you can develop and
try it without a Quest.

![MobileCAD XR: the feature tree on the left, the tool bars in front of you, a feature dialog on the right, the part in the middle](docs/ui-around-you.png)

## Modelling in the headset

MobileCAD XR is MobileCAD, with its 3D view taken out of the screen and put into the room, and its interface laid out
around you like a desk with three monitors, with nothing behind it, so the model shows through wherever the UI draws nothing:

- **left:** the feature tree, turned towards you;
- **ahead, under the model:** the top bar, the tool strip (or the sketch tools) and the workbench tabs, tilted up towards
  your hands like a keyboard; banners, selection chips, menus and dialogs float above it, over the model;
- **right:** the docked panels: tool panels, feature dialogs, task panels, the sketch's constraint list, the number pad.

Every window can be moved on its own: grip it and put it where you like (A / X puts them all back). Menus and dialogs
are shown whole, each on its own surface. When a text field has focus (tool search, names, text prompts, spreadsheet
cells) a keyboard opens under the view area: point and click its keys.

**Passthrough** (Quest): your room shows behind the model and the windows, where they draw nothing. The scale badge's
**PASSTHROUGH** button switches it on and off (on by default); off, you work in a dark room with a floor grid.

On the headset the windows are compositor layers: the headset samples them once, straight at display resolution
(supersampled and sharpened), instead of through the eye images, so text is as crisp as the display allows. The scene is
drawn with 4x anti-aliasing at 1.3x the runtime's recommended resolution. While you sketch, the constraint symbols, dimensions, sizes and snap hints face you at a readable size,
wherever you look at the sketch from.

The model stands on a **build plate**: a see-through tabletop on its ground plane (FreeCAD's XY plane) around the
3D origin, with a grid (its spacing printed on the rim) and the X / Y / Z axes. It is the table you work on: grab its rim
with the grip to pick it up and carry it (it turns with your wrist and stays level), or point at the rim and pull the
trigger to slide it around. A new document keeps the table where you put it; an empty one shows just the plate, ready
for a first sketch.

![The build plate: the model on its origin, the tool panel and the tree around it](docs/build-plate.png)

It is the whole app: every workbench and tool (sketcher, Part Design, fillets and chamfers, patterns, booleans,
assemblies, FEM, drawings), feature dialogs with the number pad, the feature tree, undo, files. Menus and dialogs close
when you click outside them, the model included, as on a touch screen. The model **is** the 3D view: the controller ray goes through MobileCAD's own view code, so everything you do with a
finger on the phone you do in 3D with the trigger, with the same tools and the same results:

- **Pick** faces, edges and vertices (only visible ones, as in FreeCAD); pointing pre-selects, like a mouse on desktop.
- **Sketch on the model**: open a sketch on a face and draw on it in the room. Every sketch tool works there with the
  sketcher's own snapping (part vertices, coincident, horizontal / vertical, on curve) and shows its own constraints,
  dimensions and live sizes, drawn on the sketch plane while the model fades.
- **Draft tools** take their points in the room through the Draft snapper (working plane, grid, object snaps).
- **Drag arrows** (Pad / Pocket length and other quantities) are dragged in 3D.

![Sketching a rectangle on a face of the part, in the room](docs/sketch-in-room.png)
![Padding it and dragging its length arrow in 3D](docs/pad-in-room.png)

| Controller | On the interface | In the room (the model, the space around it) |
|---|---|---|
| Trigger | Tap / drag, like a finger | Like a finger on MobileCAD's view: tap picks, places sketch points and Draft points; drag drags (sketch geometry, dimension labels, arrows); double tap clears the selection |
| Pointing | Hover | Pre-selection, and the sketch / Draft tool's preview follows the ray |
| Grip | Move that window | Grab the model: move and turn it with your hand · on the build plate's rim: pick the table up and carry it |
| Trigger on the build plate's rim | | Slide the table where the ray points |
| Both grips | | Zoom (pinch: pull your hands apart / together) and turn the model between your hands |
| Thumbstick | Scroll | Up/down scales, left/right turns (about the origin) |
| B / Y | | The view's context menu (a two-finger tap on the phone) |
| A / X | Put the windows back and the table in front of you | |
| Scale badge (under the tool bars) | **TRUE SIZE**: the model at its real size, 1 mm = 1 mm · **FIT VIEW**: back to the scaled view · **PASSTHROUGH**: your room on / off | |

With passthrough off, your controllers are shown in your hands as Quest 3 Touch Plus models, their trigger, grip, stick
and buttons moving as you use them, and your hands as a hand model. In passthrough you see your real controllers and
hands instead (the cameras' view of things that close is reprojected a few centimetres off, so a model laid over them
never lines up); only the rays are drawn.

![The Quest 3 controllers in the room](docs/controllers.png)

### Hands

Put the controllers down and use your hands: the headset tracks them and shows a hand model on your hands, every finger
following yours. Everything works the same, with the gestures standing in for the buttons:

| Hand | Does what |
|---|---|
| Point, then pinch (thumb + index) | The trigger: tap = click, pinch and move = drag, on the interface and on the model / the view. A ray from your shoulder through your hand shows where you point |
| Touch a window with your index finger | Tap / drag it directly, like a touch screen (move the windows close to use them this way) |
| Fist (close your hand for a moment) | The grip: grab the model, a window or the build plate's rim and carry it. A pinch never turns into a grab |
| Both hands pinching (or two fists) | Zoom and turn the model between your hands: pull them apart to grow it |
| Pinch thumb + middle finger | The view's context menu (B / Y) |
| Left palm towards you, pinch | Menu (A / X): put the windows back and the table in front of you |

Pinch and grab are the headset's own (the same detection its system menus use, through `XR_EXT_hand_interaction`), and
the ray is its own pointer (`XR_FB_hand_tracking_aim`); on runtimes without them they are read from the finger joints.
The hands and the rays are drawn in a layer of their own over the windows, so a finger touching a window is seen in
front of it; behind the model they are hidden by it.

![Hands: the left one pointing at a window, the right one pinching](docs/hands.png)

### True size

The badge under the tool bars always shows the model's scale (`SCALE 1:4.2`, `16:1`). **TRUE SIZE** switches to 1:1:
a small part comes to ~40 cm in front of you to look at closely (a 25 mm bit really is 25 mm), a part bigger than arm's reach is stood on
the floor in front of you so you can walk around it (a building at full size). In true size the scaling gestures only
move and turn the model, so the scale can't drift; **FIT VIEW** puts the model back where and how big it was. Free
scaling with both hands also snaps to 1:1 as you pass it.

![The BIM example at true size, standing on the floor](docs/true-size.png)

A typical flow: point at a face and click it, tap **Sketch → New Sketch** on the tool bar, pick Rectangle and click two
corners on the face, close the sketch (✓), tap **Pad**, then drag its arrow out of the part (or type the length on the
number pad). FreeCAD's example projects ship
with the app (Part Design, Assembly, FEM, BIM, Part); open your own `.FCStd` files from the ☰ menu.

**Install on a Quest 3:** enable developer mode (Meta Horizon app → Devices → Developer mode), connect the headset by
USB, then `adb install MobileCAD-XR-quest.apk` (or drag the APK into Meta Quest Developer Hub). It appears under
*Library → Unknown sources*.

## Test without a headset

Two ways, covering different parts:

- **The whole app, on the Android emulator** (or a phone): without an OpenXR runtime MobileCAD XR shows a preview of
  the XR scene: MobileCAD's full UI around you and the model in 3D. One finger is the controller with its trigger pressed
  (on the UI, and on the model / in the space around it: pick, sketch, place points, drag arrows); two fingers look
  around. This exercises everything except the OpenXR runtime itself
  (`./gradlew :app:assembleX86Debug`, install, run).
- **The OpenXR path, on a simulated headset** (below).

### Simulated headset

`sim/` is a Docker image with **Monado**, the open-source OpenXR runtime, using its *qwerty* driver: a simulated headset
and two controllers driven by keyboard and mouse, on a virtual display with software rendering (no GPU needed). The
desktop build of MobileCAD XR (`desktop/`) runs the **same XR core** as the Quest app against it.

```sh
sim/run.sh          # then open http://localhost:6080/vnc.html and press Connect
sim/run.sh --test   # automated: scripted controller picks on the model, grabs and moves it, clicks a panel button, switches true size on and off;
                    # then the same with tracked hands (pinch, fist, a finger pressing the button) and a two-hand pinch zoom
sim/run.sh --hands  # interactive, the controllers shown as hands: trigger = pinch, grip = fist, B / Y = middle-finger pinch
```

In the browser, click into the **Monado!** panel on the right, then:
`W A S D` / `Q E` move the headset · arrows or right-drag look · hold `Ctrl` = left controller, hold `Alt` = right
controller · left click = trigger (click selects, hold grabs) · middle click = grip · `N` = menu (next file) ·
`T F G H` = thumbstick.

MobileCAD's Android UI cannot run on Linux, so the desktop build shows a small stand-in panel with real buttons to test
the panel interaction. The scenes it loads (`scenes/*.mcxr`) are FreeCAD's examples exported by MobileCAD's engine
(`tools/export_scenes.py`, run inside the phone app with `mobilecad/engine/fcrun.sh`).

## How it is built

| Path | What |
|---|---|
| `core/` | The XR core, shared by both builds: OpenXR session, controller actions and hand trackers (`xr_session`), interaction (`interaction`), hand gestures (`hands`), the hand / controller glTF models (`gltf_model`), GL renderer for OpenGL ES 3 and desktop GL 3.3 (`renderer`), FreeCAD meshes and face picking (`scene`) |
| `android/` | The headset app (Gradle). Compiles all of MobileCAD from the submodule; `XrActivity.kt` runs MobileCAD's Compose UI (its XR layout) on a see-through virtual display whose SurfaceTexture is cut into the surfaces around you (`arrangeSheets`; the image's alpha, read back a few times a second, decides where a ray hits the UI or passes through to the model), turns controller rays into touches on the UI and into MobileCAD's view (`ViewportView.xrPointer`: a virtual camera
looking down the ray), and sends what that view draws (the model, preselection, overlay, the sketch rendered by its own editor onto a texture on the sketch plane) to the XR scene; `xr_android.cpp` runs the core on an OpenXR thread (or the touch preview). Flavors: `arm64` (Quest), `x86` (emulator) |
| `desktop/` | Linux build of the core against any OpenXR runtime (Monado), with a mirror window, `--screenshot` and `--autotest` |
| `sim/` | The headset-free test bench (Monado + Xvfb + noVNC in Docker) |
| `mobilecad/` | [MobileCAD](https://github.com/LucasCoraca/mobilecad) as a git submodule: the FreeCAD engine, its build scripts and runtime packaging |

Build the Quest APK (after building MobileCAD's native runtime, see its README):

```sh
git submodule update --init
python3 -I mobilecad/engine/package_runtime.py arm64-v8a android
cd android && ./gradlew :app:assembleArm64Release
```

## License

GPL-3.0-or-later, like MobileCAD. Includes FreeCAD (LGPL-2.1-or-later), the Khronos OpenXR loader (Apache-2.0), the
hand and controller models of the WebXR Input Profiles (MIT, `assets/models/LICENSE-webxr-input-profiles.md`), cgltf (MIT)
and stb_image (public domain) in `core/third_party`, and
MobileCAD's other components (see MobileCAD's About ▸ Open-source licenses). The test bench uses Monado (Boost
Software License 1.0), installed from Ubuntu's packages. MobileCAD XR is an independent project, not affiliated with,
sponsored or endorsed by the FreeCAD project, Meta or the Khronos Group. FreeCAD is a trademark of the FreeCAD Project
Association; Meta Quest is a trademark of Meta Platforms; OpenXR is a trademark of the Khronos Group.
