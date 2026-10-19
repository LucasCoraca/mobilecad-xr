// The CAD model shown in XR: one mesh per FreeCAD object, in the engine's tessellation format (mobilecad.bridge.tessellate),
// so faces/edges keep their FreeCAD ids (Face<i> / Edge<i>) for picking.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "xmath.h"

namespace mcxr {

struct Mesh {
    std::string name;
    std::vector<float> tris;    // x y z nx ny nz faceId   per vertex, 3 vertices per triangle
    std::vector<float> edges;   // x1 y1 z1 x2 y2 z2 edgeId   per segment (7 floats)
    float color[3] = {0.81f, 0.84f, 0.86f};
    std::vector<int> faceFirst, faceCount;         // per FreeCAD face id (1-based): its vertex range in tris
    unsigned vao = 0, vbo = 0, evao = 0, evbo = 0;   // GL objects, made by the renderer
    size_t triVerts() const { return tris.size() / 7; }
    size_t edgeSegments() const { return edges.size() / 7; }
};

struct Hit { int mesh = -1; int face = 0; float t = 0; V3 point; };

struct Scene {
    std::string title;
    std::vector<Mesh> meshes;
    V3 min{1e30f, 1e30f, 1e30f}, max{-1e30f, -1e30f, -1e30f};
    unsigned version = 0;       // bumped on change; the renderer re-uploads

    /** Adds one object from the engine's tessellation bytes: [int32 n][n floats tris][int32 n][n floats edges] ... */
    bool addTessellation(const std::string& name, const uint8_t* data, size_t size, const float rgb[3]);
    void clear() { meshes.clear(); min = {1e30f, 1e30f, 1e30f}; max = {-1e30f, -1e30f, -1e30f}; ++version; }
    bool empty() const { return meshes.empty(); }
    V3 center() const { return (min + max) * 0.5f; }
    float radius() const { return len(max - min) * 0.5f; }
    /** Nearest face hit by a ray given in model coordinates. */
    Hit pick(V3 origin, V3 dir) const;

    /** Scene bundle (.mcxr): "MCXR1" u32 count, then per object: u16 nameLen, name, 3 x f32 color, u32 size, tessellation. */
    bool loadBundle(const std::string& path);
    static bool writeBundle(const std::string& path, const Scene& s);
};

}  // namespace mcxr
