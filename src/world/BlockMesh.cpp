#include "world/BlockMesh.h"

namespace mc {
namespace {

struct FaceVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

// Four corners per face of the unit cube (CCW from outside); triangles (0,1,2),(0,2,3).
// UVs put v=0 at the top of the block for side faces.
const FaceVertex kFace[6][4] = {
    // +X (east)
    {{{1, 1, 1}, {1, 0}}, {{1, 0, 1}, {1, 1}}, {{1, 0, 0}, {0, 1}}, {{1, 1, 0}, {0, 0}}},
    // -X (west)
    {{{0, 1, 0}, {1, 0}}, {{0, 0, 0}, {1, 1}}, {{0, 0, 1}, {0, 1}}, {{0, 1, 1}, {0, 0}}},
    // +Y (top)
    {{{0, 1, 0}, {0, 0}}, {{0, 1, 1}, {0, 1}}, {{1, 1, 1}, {1, 1}}, {{1, 1, 0}, {1, 0}}},
    // -Y (bottom)
    {{{0, 0, 1}, {0, 0}}, {{0, 0, 0}, {0, 1}}, {{1, 0, 0}, {1, 1}}, {{1, 0, 1}, {1, 0}}},
    // +Z (south)
    {{{0, 1, 1}, {0, 0}}, {{0, 0, 1}, {0, 1}}, {{1, 0, 1}, {1, 1}}, {{1, 1, 1}, {1, 0}}},
    // -Z (north)
    {{{1, 1, 0}, {0, 0}}, {{1, 0, 0}, {0, 1}}, {{0, 0, 0}, {1, 1}}, {{0, 1, 0}, {1, 0}}},
};

// Directional shading (top brightest, bottom darkest) for a basic lit look.
const float kFaceLight[6] = {0.62f, 0.62f, 1.0f, 0.5f, 0.8f, 0.8f};

// Which world axes feed each face's texture axes (u-axis, v-axis).
const int kFaceUAxis[6] = {2, 2, 0, 0, 0, 0}; // X-faces sample u from z; others from x
const int kFaceVAxis[6] = {1, 1, 2, 2, 1, 1}; // side faces sample v from y; caps from z

void appendQuad(ChunkMeshData& mesh, const glm::vec3 p[4], const glm::vec2 uv[4],
                float layer, float light) {
    uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    for (int k = 0; k < 4; ++k) {
        ChunkVertex v;
        v.pos = p[k];
        v.uv = uv[k];
        v.layer = layer;
        v.light = light;
        mesh.vertices.push_back(v);
    }
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
}

} // namespace

const int kFaceNeighbor[6][3] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
};

void emitBox(ChunkMeshData& mesh, const glm::vec3& origin, const glm::vec3& from,
             const glm::vec3& to, const std::array<uint32_t, 6>& layers, uint8_t faceMask) {
    glm::vec3 size = to - from;
    for (int face = 0; face < 6; ++face) {
        if (!(faceMask & faceBit(face))) continue;
        glm::vec3 p[4];
        glm::vec2 uv[4];
        int ua = kFaceUAxis[face];
        int va = kFaceVAxis[face];
        for (int k = 0; k < 4; ++k) {
            glm::vec3 unit = kFace[face][k].pos;
            p[k] = origin + from + unit * size;
            // Remap the unit UVs to the sub-box extents along the face's texture axes so
            // partial boxes sample the matching slice of the block texture. The unit
            // table encodes each face's u/v orientation: when the corner's uv matches
            // its position along the axis, texture coords grow with the axis; otherwise
            // they're mirrored.
            glm::vec2 t = kFace[face][k].uv;
            float posU = unit[ua];
            float cu = from[ua] + posU * (to[ua] - from[ua]);
            uv[k].x = (t.x == posU) ? cu : 1.0f - cu;
            float posV = unit[va];
            float cv = from[va] + posV * (to[va] - from[va]);
            if (va == 1) {
                uv[k].y = 1.0f - cv; // side faces: v=0 at the top of the block
            } else {
                uv[k].y = (t.y == posV) ? cv : 1.0f - cv;
            }
        }
        appendQuad(mesh, p, uv, static_cast<float>(layers[face]), kFaceLight[face]);
    }
}

void emitCross(ChunkMeshData& mesh, const glm::vec3& origin, uint32_t layer) {
    const glm::vec3 o = origin;
    glm::vec2 uv[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    glm::vec3 q1[4] = {o + glm::vec3(0, 1, 0), o + glm::vec3(0, 0, 0),
                       o + glm::vec3(1, 0, 1), o + glm::vec3(1, 1, 1)};
    glm::vec3 q2[4] = {o + glm::vec3(1, 1, 0), o + glm::vec3(1, 0, 0),
                       o + glm::vec3(0, 0, 1), o + glm::vec3(0, 1, 1)};
    appendQuad(mesh, q1, uv, static_cast<float>(layer), 1.0f);
    appendQuad(mesh, q2, uv, static_cast<float>(layer), 1.0f);
}

} // namespace mc
