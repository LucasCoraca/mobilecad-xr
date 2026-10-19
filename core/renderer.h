// Draws the XR scene for one eye: the CAD model (lit faces, dark edges, hover / selection highlights on faces and edges),
// a floor grid, controller rays, cursor dots and textured panels (the app's UI, the info card). The same code runs on
// OpenGL ES 3 (headsets) and desktop GL 3.3 (simulator); on Android a panel can show an external (SurfaceTexture) image.
#pragma once
#include <vector>
#include "gl.h"
#include "scene.h"
#include "xmath.h"

namespace mcxr {

/** A coloured line list: x y z r g b per vertex, two vertices per segment. */
struct Lines { std::vector<float> v; void add(V3 a, V3 b, V3 c) { for (V3 p : {a, b}) { v.insert(v.end(), {p.x, p.y, p.z, c.x, c.y, c.z}); } } };

class Renderer {
public:
    bool init();
    void upload(Scene& s);                                   // (re)creates the mesh buffers when the scene changed
    void beginView(GLuint fbo, int x, int y, int w, int h, bool clear);
    void drawScene(const Scene& s, const Pose& model, const M4& viewProj, V3 eyePos, ElemRef hover, const std::vector<ElemRef>& selected);
    void drawLines(const Lines& l, const M4& viewProj);
    /** Points (x y z r g b; the second vertex of each Lines pair is ignored) drawn as round dots of `sizePx` pixels. */
    void drawDots(const std::vector<float>& pts, float sizePx, const M4& viewProj);
    /** A textured rectangle centred on `center` (local X = width, local Y = height), corners rounded by `radius` (m). */
    void drawPanel(GLuint tex, bool external, const Pose& center, float w, float h, float radius, const M4& viewProj, float dim = 1.f);
    GLuint makeTexture(int w, int h, const void* rgba, GLuint reuse = 0);
    /** Forget GL objects of a previous context (they are gone with it): the next upload starts afresh. */
    void contextLost() { uploaded = ~0u; uploadedScene = nullptr; buffers.clear(); arrays.clear(); }
    float clear[3] = {0.11f, 0.13f, 0.16f};
    /** 2.2 when drawing into an sRGB swapchain (colours here are sRGB values, the target expects linear), 1 otherwise. */
    float gamma = 1.f;
private:
    GLuint meshProg = 0, lineProg = 0, texProg = 0, oesProg = 0, lineVao = 0, lineVbo = 0, quadVao = 0, quadVbo = 0;
    GLint uMvp = -1, uModel = -1, uColor = -1, uLit = -1, uEye = -1;
    unsigned uploaded = ~0u; const Scene* uploadedScene = nullptr;
    std::vector<GLuint> buffers, arrays;                    // owned by the last upload
};

}  // namespace mcxr
