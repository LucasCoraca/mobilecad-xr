#include "renderer.h"
#include <cmath>
#include <cstdio>
#include <string>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_PROGRAM_POINT_SIZE
#define GL_PROGRAM_POINT_SIZE 0x8642
#endif

namespace mcxr {

static GLuint compile(GLenum type, const std::string& body, const char* header = MCXR_GLSL_HEADER) {
    std::string src = std::string(header) + body;
    GLuint s = glCreateShader(type); const char* p = src.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "mcxr shader: %s\n", log); }
    return s;
}
static GLuint program(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram(); glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p); GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log); std::fprintf(stderr, "mcxr link: %s\n", log); return 0; }
    return p;
}

// faces: two-sided lighting from a key light above the viewer and a fill light, plus a little ambient
static const char* kMeshVs = R"(
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm;
uniform mat4 uMvp; uniform mat4 uModel; out vec3 vN; out vec3 vP;
void main(){ vN = mat3(uModel) * aNrm; vP = (uModel * vec4(aPos,1.0)).xyz; gl_Position = uMvp * vec4(aPos,1.0); })";
static const char* kMeshFs = R"(
in vec3 vN; in vec3 vP; uniform vec4 uColor; uniform float uLit; uniform vec3 uEye; uniform float uGamma; out vec4 o;
void main(){ vec3 n = normalize(vN); if (dot(n, uEye - vP) < 0.0) n = -n;
  float l = 0.38 + 0.47*max(dot(n, normalize(vec3(0.35,0.9,0.45))),0.0) + 0.22*max(dot(n, normalize(vec3(-0.6,0.2,-0.5))),0.0);
  o = vec4(pow(uColor.rgb * mix(1.0, l, uLit), vec3(uGamma)), uColor.a); })";
static const char* kLineVs = R"(
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aCol; uniform mat4 uMvp; uniform float uSize; out vec3 vC;
void main(){ vC = aCol; gl_PointSize = uSize; gl_Position = uMvp * vec4(aPos,1.0); })";
static const char* kLineFs = R"(in vec3 vC; uniform float uGamma; uniform float uRound; out vec4 o;
void main(){ if (uRound > 0.5 && length(gl_PointCoord - vec2(0.5)) > 0.5) discard; o = vec4(pow(vC, vec3(uGamma)),1.0); })";
// panels: rounded corners (uSize = panel size in m, uRadius in m), optional dimming
static const char* kTexVs = R"(
layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUv; uniform mat4 uMvp; out vec2 vUv;
void main(){ vUv = aUv; gl_Position = uMvp * vec4(aPos,1.0); })";
static const char* kPanelMask = R"(
uniform vec2 uSize; uniform float uRadius; uniform float uGamma; uniform float uDim; out vec4 o;
float mask(vec2 uv){ vec2 p = abs((uv - 0.5) * uSize) - (uSize * 0.5 - vec2(uRadius)); return length(max(p, 0.0)) - uRadius; }
vec4 finish(vec4 c){ if (mask(vUv) > 0.0) discard; return vec4(pow(c.rgb * uDim, vec3(uGamma)), c.a); })";
static const char* kTexFs = R"(in vec2 vUv; uniform sampler2D uTex;
%MASK%
void main(){ o = finish(texture(uTex, vUv)); })";
static const char* kOesFs = R"(in vec2 vUv; uniform samplerExternalOES uTex;
%MASK%
void main(){ o = finish(vec4(texture(uTex, vUv).rgb, 1.0)); })";

static std::string withMask(const char* fs) { std::string s = fs; s.replace(s.find("%MASK%"), 6, kPanelMask); return s; }

bool Renderer::init() {
    GLuint lineVs = compile(GL_VERTEX_SHADER, kLineVs), texVs = compile(GL_VERTEX_SHADER, kTexVs);
    meshProg = program(compile(GL_VERTEX_SHADER, kMeshVs), compile(GL_FRAGMENT_SHADER, kMeshFs));
    lineProg = program(lineVs, compile(GL_FRAGMENT_SHADER, kLineFs));
    texProg = program(texVs, compile(GL_FRAGMENT_SHADER, withMask(kTexFs)));
#ifdef __ANDROID__
    oesProg = program(texVs, compile(GL_FRAGMENT_SHADER, withMask(kOesFs),
                      "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;\n"));
#endif
    uMvp = glGetUniformLocation(meshProg, "uMvp"); uModel = glGetUniformLocation(meshProg, "uModel");
    uColor = glGetUniformLocation(meshProg, "uColor"); uLit = glGetUniformLocation(meshProg, "uLit"); uEye = glGetUniformLocation(meshProg, "uEye");
    glGenVertexArrays(1, &lineVao); glGenBuffers(1, &lineVbo);
    glBindVertexArray(lineVao); glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)12);
    float quad[] = {-.5f, -.5f, 0, 0, 1,  .5f, -.5f, 0, 1, 1,  .5f, .5f, 0, 1, 0,  -.5f, -.5f, 0, 0, 1,  .5f, .5f, 0, 1, 0,  -.5f, .5f, 0, 0, 0};
    glGenVertexArrays(1, &quadVao); glGenBuffers(1, &quadVbo);
    glBindVertexArray(quadVao); glBindBuffer(GL_ARRAY_BUFFER, quadVbo); glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void*)12);
    glBindVertexArray(0);
#ifndef __ANDROID__
    glEnable(GL_PROGRAM_POINT_SIZE);
#endif
    return meshProg && lineProg && texProg;
}

void Renderer::upload(Scene& s) {
    if (uploaded == s.version && uploadedScene == &s) return;
    if (!buffers.empty()) glDeleteBuffers((GLsizei)buffers.size(), buffers.data());
    if (!arrays.empty()) glDeleteVertexArrays((GLsizei)arrays.size(), arrays.data());
    buffers.clear(); arrays.clear();
    for (auto& m : s.meshes) {
        glGenVertexArrays(1, &m.vao); glGenBuffers(1, &m.vbo); arrays.push_back(m.vao); buffers.push_back(m.vbo);
        glBindVertexArray(m.vao); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, m.tris.size() * 4, m.tris.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 28, (void*)12);
        // edges: segments of 7 floats -> two xyz vertices each
        std::vector<float> ev; ev.reserve(m.edgeSegments() * 6);
        for (size_t i = 0; i + 6 < m.edges.size(); i += 7) ev.insert(ev.end(), m.edges.begin() + i, m.edges.begin() + i + 6);
        glGenVertexArrays(1, &m.evao); glGenBuffers(1, &m.evbo); arrays.push_back(m.evao); buffers.push_back(m.evbo);
        glBindVertexArray(m.evao); glBindBuffer(GL_ARRAY_BUFFER, m.evbo);
        glBufferData(GL_ARRAY_BUFFER, ev.size() * 4, ev.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void*)0);
        glDisableVertexAttribArray(1);
    }
    glBindVertexArray(0); uploaded = s.version; uploadedScene = &s;
}

void Renderer::beginView(GLuint fbo, int x, int y, int w, int h, bool doClear) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo); glViewport(x, y, w, h);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDisable(GL_CULL_FACE);
    if (doClear) {
        glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h);
        glClearColor(std::pow(clear[0], gamma), std::pow(clear[1], gamma), std::pow(clear[2], gamma), 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
}

void Renderer::drawScene(const Scene& s, const Pose& model, const M4& vp, V3 eye, ElemRef hover, const std::vector<ElemRef>& sel) {
    M4 mm = fromPose(model); M4 mvp = mul(vp, mm);
    glUseProgram(meshProg);
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, mvp.m); glUniformMatrix4fv(uModel, 1, GL_FALSE, mm.m); glUniform3f(uEye, eye.x, eye.y, eye.z);
    glUniform1f(glGetUniformLocation(meshProg, "uGamma"), gamma);
    const V3 hoverCol{0.73f, 0.86f, 0.96f}, selCol{0.26f, 0.56f, 0.87f};
    auto faceRange = [&](const Mesh& m, int id, int& first, int& count) {
        if (id <= 0 || id >= (int)m.faceFirst.size() || m.faceFirst[id] < 0) return false;
        first = m.faceFirst[id]; count = m.faceCount[id]; return true;
    };
    glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(1.f, 2.f);
    for (size_t i = 0; i < s.meshes.size(); ++i) {
        const Mesh& m = s.meshes[i]; if (!m.triVerts()) continue;
        glBindVertexArray(m.vao); glEnableVertexAttribArray(1);
        glUniform1f(uLit, 1.f); glUniform4f(uColor, m.color[0], m.color[1], m.color[2], 1.f);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m.triVerts());
        int f, c;
        if (hover.mesh == (int)i && hover.kind == 'F' && faceRange(m, hover.id, f, c)) { glUniform4f(uColor, hoverCol.x, hoverCol.y, hoverCol.z, 1.f); glDrawArrays(GL_TRIANGLES, f, c); }
        for (const ElemRef& r : sel)
            if (r.mesh == (int)i && r.kind == 'F' && faceRange(m, r.id, f, c)) { glUniform4f(uColor, selCol.x, selCol.y, selCol.z, 1.f); glDrawArrays(GL_TRIANGLES, f, c); }
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glUniform1f(uLit, 0.f); glVertexAttrib3f(1, 0.f, 0.f, 1.f);                       // edges carry no normals
    for (size_t i = 0; i < s.meshes.size(); ++i) {
        const Mesh& m = s.meshes[i]; if (!m.edgeSegments()) continue;
        glBindVertexArray(m.evao);
        glUniform4f(uColor, 0.17f, 0.21f, 0.25f, 1.f); glLineWidth(1.f);
        glDrawArrays(GL_LINES, 0, (GLsizei)(m.edgeSegments() * 2));
        auto edge = [&](int id, V3 col) {
            if (id <= 0 || id >= (int)m.edgeFirst.size() || m.edgeFirst[id] < 0) return;
            glUniform4f(uColor, col.x, col.y, col.z, 1.f); glLineWidth(4.f);
            glDrawArrays(GL_LINES, m.edgeFirst[id] * 2, m.edgeCount[id] * 2);
        };
        if (hover.mesh == (int)i && hover.kind == 'E') edge(hover.id, V3{0.40f, 0.78f, 0.79f});
        for (const ElemRef& r : sel) if (r.mesh == (int)i && r.kind == 'E') edge(r.id, selCol);
    }
    glLineWidth(1.f); glBindVertexArray(0);
}

void Renderer::drawLines(const Lines& l, const M4& vp) {
    if (l.v.empty()) return;
    glUseProgram(lineProg); glUniformMatrix4fv(glGetUniformLocation(lineProg, "uMvp"), 1, GL_FALSE, vp.m);
    glUniform1f(glGetUniformLocation(lineProg, "uGamma"), gamma); glUniform1f(glGetUniformLocation(lineProg, "uRound"), 0.f);
    glUniform1f(glGetUniformLocation(lineProg, "uSize"), 1.f);
    glBindVertexArray(lineVao); glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glBufferData(GL_ARRAY_BUFFER, l.v.size() * 4, l.v.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, (GLsizei)(l.v.size() / 6)); glBindVertexArray(0);
}

void Renderer::drawDots(const std::vector<float>& pts, float sizePx, const M4& vp) {
    if (pts.empty()) return;
    glUseProgram(lineProg); glUniformMatrix4fv(glGetUniformLocation(lineProg, "uMvp"), 1, GL_FALSE, vp.m);
    glUniform1f(glGetUniformLocation(lineProg, "uGamma"), gamma); glUniform1f(glGetUniformLocation(lineProg, "uRound"), 1.f);
    glUniform1f(glGetUniformLocation(lineProg, "uSize"), sizePx);
    glBindVertexArray(lineVao); glBindBuffer(GL_ARRAY_BUFFER, lineVbo);
    glBufferData(GL_ARRAY_BUFFER, pts.size() * 4, pts.data(), GL_STREAM_DRAW);
    glDisable(GL_DEPTH_TEST); glDrawArrays(GL_POINTS, 0, (GLsizei)(pts.size() / 6)); glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

void Renderer::drawPanel(GLuint tex, bool external, const Pose& c, float w, float h, float radius, const M4& vp, float dim) {
    if (!tex || (external && !oesProg)) return;
    M4 m = fromPose(c); for (int k = 0; k < 3; ++k) { m.m[k] *= w; m.m[4 + k] *= h; }
    M4 mvp = mul(vp, m);
    GLuint prog = external ? oesProg : texProg; GLenum target = external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    glUseProgram(prog); glUniformMatrix4fv(glGetUniformLocation(prog, "uMvp"), 1, GL_FALSE, mvp.m); glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUniform1f(glGetUniformLocation(prog, "uGamma"), gamma); glUniform2f(glGetUniformLocation(prog, "uSize"), w, h);
    glUniform1f(glGetUniformLocation(prog, "uRadius"), radius); glUniform1f(glGetUniformLocation(prog, "uDim"), dim);
    glActiveTexture(GL_TEXTURE0); glBindTexture(target, tex);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(quadVao); glDrawArrays(GL_TRIANGLES, 0, 6); glBindVertexArray(0);
    glDisable(GL_BLEND);
}

GLuint Renderer::makeTexture(int w, int h, const void* rgba, GLuint reuse) {
    GLuint t = reuse; if (!t) glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

}  // namespace mcxr
