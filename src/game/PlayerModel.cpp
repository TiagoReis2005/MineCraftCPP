#include "game/PlayerModel.h"

#include "anim/Json.h"
#include "core/Paths.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace mc {
namespace {

constexpr float kSkin = 64.0f; // skin is 64x64

void appendFace(ModelData& m, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                float u0, float v0, float u1, float v1, float shade) {
    uint32_t base = static_cast<uint32_t>(m.vertices.size());
    m.vertices.push_back({a, {u0 / kSkin, v0 / kSkin}, shade});
    m.vertices.push_back({b, {u1 / kSkin, v0 / kSkin}, shade});
    m.vertices.push_back({c, {u1 / kSkin, v1 / kSkin}, shade});
    m.vertices.push_back({d, {u0 / kSkin, v1 / kSkin}, shade});
    m.indices.push_back(base + 0);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
    m.indices.push_back(base + 0);
    m.indices.push_back(base + 2);
    m.indices.push_back(base + 3);
}

// One face of a Bedrock per-face UV block: {"uv": [u,v], "uv_size": [w,h]}.
// A face absent from the file is NOT meshed (Blockbench "disabled" faces — the
// split-limb geometry omits the internal elbow/knee seam faces this way).
struct FaceUV {
    bool present = false;
    glm::vec2 uv{0.0f};
    glm::vec2 size{0.0f}; // may be negative = flipped sampling (vanilla down faces)
};
struct PerFaceUV {
    FaceUV north, south, east, west, up, down; // Bedrock face names
};

// Adds a box in pixel space with the Minecraft box-UV unwrap (net order:
// right | front | left | back along U). Origin = min corner (before inflate).
// `inflate` grows the geometry outward on every axis but keeps the same UV region,
// which is how MC draws the outer/overlay shell over the base layer.
//
// Convention: model faces -Z, character's right is +X. Net cells along U are
// right(+X) | front(-Z) | left(-X) | back(+Z). Front/back map +X->min-U; the East
// (+X) face uses the leftmost "right" cell and West (-X) the "left" cell. Top and
// bottom have their U reversed so they line up with the band faces.
//
// `mirror` = Bedrock's cube mirror flag: the texture flips horizontally (each face's
// U reversed, and the two side cells swap) — Alex's left leg reuses the right-leg
// texture region this way.
//
// `pf` (optional) = Bedrock per-face UVs. Each face's rect replaces the box-UV cell
// in the SAME slot orientation, so per-face data that matches the box unwrap renders
// pixel-identical (verified against the Blockbench export of the unsplit limbs).
// Slot orientations relative to the raw rect [uv .. uv+size]:
//   band faces (north/south/east/west): as-is; up: U and V flipped; down: V flipped
//   (vanilla exports store down faces with negative uv_size, which lands upright here).
// Absent faces are skipped. `mirror` is ignored — exporters bake it into the rects.
void addBox(ModelData& m, float ox, float oy, float oz, float w, float h, float d,
            float tu, float tv, float inflate = 0.0f, bool mirror = false,
            const PerFaceUV* pf = nullptr) {
    float x0 = ox - inflate, y0 = oy - inflate, z0 = oz - inflate;
    float x1 = ox + w + inflate, y1 = oy + h + inflate, z1 = oz + d + inflate;

    enum Slot { Band, FlipUV, FlipV };
    auto face = [&](const FaceUV* f, int slot, glm::vec3 a, glm::vec3 b, glm::vec3 c,
                    glm::vec3 dd, float u0, float v0, float u1, float v1, float shade) {
        if (pf) {
            if (!f->present) return; // disabled face
            glm::vec2 A = f->uv, B = f->uv + f->size;
            switch (slot) {
                case Band:   u0 = A.x; v0 = A.y; u1 = B.x; v1 = B.y; break;
                case FlipUV: u0 = B.x; v0 = B.y; u1 = A.x; v1 = A.y; break;
                case FlipV:  u0 = A.x; v0 = B.y; u1 = B.x; v1 = A.y; break;
            }
        } else if (mirror) {
            std::swap(u0, u1);
        }
        appendFace(m, a, b, c, dd, u0, v0, u1, v1, shade);
    };
    float eastU = mirror ? tu + d + w : tu; // the side cells swap when mirrored
    float westU = mirror ? tu : tu + d + w;

    // Front (-Z / north).
    face(pf ? &pf->north : nullptr, Band, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z0}, {x1, y0, z0},
         tu + d, tv + d, tu + d + w, tv + d + h, 0.9f);
    // Back (+Z / south).
    face(pf ? &pf->south : nullptr, Band, {x0, y1, z1}, {x1, y1, z1}, {x1, y0, z1}, {x0, y0, z1},
         tu + d + w + d, tv + d, tu + d + w + d + w, tv + d + h, 0.9f);
    // East (+X, character's right).
    face(pf ? &pf->east : nullptr, Band, {x1, y1, z1}, {x1, y1, z0}, {x1, y0, z0}, {x1, y0, z1},
         eastU, tv + d, eastU + d, tv + d + h, 0.75f);
    // West (-X, character's left).
    face(pf ? &pf->west : nullptr, Band, {x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}, {x0, y0, z0},
         westU, tv + d, westU + d, tv + d + h, 0.75f);
    // Top (+Y): U matches the band; V flipped so the -Z (front) edge meets the
    // front face at the bottom of the top cell.
    face(pf ? &pf->up : nullptr, FlipUV, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1},
         tu + d + w, tv + d, tu + d, tv, 1.0f);
    // Bottom (-Y): +X -> max U, -Z -> max V (the standard MC down-cell orientation).
    face(pf ? &pf->down : nullptr, FlipV, {x0, y0, z1}, {x1, y0, z1}, {x1, y0, z0}, {x0, y0, z0},
         tu + d + w, tv, tu + d + w + w, tv + d, 0.5f);
}

// ---- Bedrock geometry (assets/models/entity/player.json) ---------------------------

glm::vec3 readVec3(const JsonValue* v, glm::vec3 def) {
    if (!v || !v->isArray() || v->array.size() < 3) return def;
    return {static_cast<float>(v->array[0].number), static_cast<float>(v->array[1].number),
            static_cast<float>(v->array[2].number)};
}

glm::vec2 readVec2(const JsonValue* v, glm::vec2 def) {
    if (!v || !v->isArray() || v->array.size() < 2) return def;
    return {static_cast<float>(v->array[0].number), static_cast<float>(v->array[1].number)};
}

bool readBool(const JsonValue* v, bool def) {
    return v && v->type == JsonValue::Type::Bool ? v->boolean : def;
}

void readFaceUV(const JsonValue* v, FaceUV* out) {
    if (!v || !v->isObject()) return; // face stays absent -> not meshed
    out->present = true;
    out->uv = readVec2(v->find("uv"), glm::vec2(0.0f));
    out->size = readVec2(v->find("uv_size"), glm::vec2(0.0f));
}

// Bedrock cube "uv" is either box UV ([u, v]) or a per-face object
// ({"north": {"uv": .., "uv_size": ..}, ...}). Returns true for the per-face form.
bool readPerFaceUV(const JsonValue* v, PerFaceUV* out) {
    if (!v || !v->isObject()) return false;
    readFaceUV(v->find("north"), &out->north);
    readFaceUV(v->find("south"), &out->south);
    readFaceUV(v->find("east"), &out->east);
    readFaceUV(v->find("west"), &out->west);
    readFaceUV(v->find("up"), &out->up);
    readFaceUV(v->find("down"), &out->down);
    return true;
}

// Overlay bone name -> its toggleable SkinLayer bit (0 = always-on base bone). Covers
// the split forearm/shin overlays too, so each half toggles with its layer.
uint32_t overlayLayerOf(const std::string& name) {
    struct Row { const char* name; uint32_t layer; };
    static const Row kRows[] = {
        {"jacket", SL_Jacket},         {"hat", SL_Hat},
        {"leftSleeve", SL_LeftSleeve}, {"leftForeSleeve", SL_LeftSleeve},
        {"rightSleeve", SL_RightSleeve}, {"rightForeSleeve", SL_RightSleeve},
        {"leftPants", SL_LeftPants},   {"leftShinPants", SL_LeftPants},
        {"rightPants", SL_RightPants}, {"rightShinPants", SL_RightPants},
    };
    for (const Row& r : kRows)
        if (name == r.name) return r.layer;
    return 0;
}

} // namespace

// Bedrock model space has +X = the character's LEFT; the engine uses +X = RIGHT.
// Boxes are mirrored across X ([x, x+w] -> [-(x+w), -x]); addBox's UV convention already
// encodes that mirror, so the texture lands correctly. Meshes go into out.bones[rigIndex]
// so they line up with Pose::compose's world transforms.
PlayerRigMeshes buildPlayerRigMeshes(const Rig& rig, bool slim, uint32_t layerMask) {
    PlayerRigMeshes out;
    out.count = rig.boneCount();
    const char* want = slim ? "geometry.npc.alex" : "geometry.npc.steve";

    JsonValue root;
    try {
        root = parseJsonFile(resolve("assets/models/entity/player.json"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[PlayerModel] player.json unreadable: %s\n", e.what());
        return out;
    }
    const JsonValue* geos = root.find("minecraft:geometry");
    if (!geos || !geos->isArray()) return out;

    for (const JsonValue& g : geos->array) {
        const JsonValue* desc = g.find("description");
        const JsonValue* id = desc ? desc->find("identifier") : nullptr;
        if (!id || !id->isString() || id->string != want) continue;

        const JsonValue* bones = g.find("bones");
        if (!bones || !bones->isArray()) return out;
        for (const JsonValue& bone : bones->array) {
            const JsonValue* nm = bone.find("name");
            if (!nm || !nm->isString()) continue;
            int idx = rig.findBone(nm->string);
            if (idx < 0 || idx >= kMaxModelBones) continue; // non-mesh / unknown bone
            if (uint32_t layer = overlayLayerOf(nm->string); layer && !(layerMask & layer))
                continue; // overlay toggled off

            bool boneMirror = readBool(bone.find("mirror"), false);
            const JsonValue* cubes = bone.find("cubes");
            if (!cubes || !cubes->isArray()) continue;
            for (const JsonValue& cube : cubes->array) {
                glm::vec3 o = readVec3(cube.find("origin"), glm::vec3(0.0f));
                glm::vec3 s = readVec3(cube.find("size"), glm::vec3(0.0f));
                PerFaceUV pf;
                bool perFace = readPerFaceUV(cube.find("uv"), &pf);
                glm::vec2 uv = perFace ? glm::vec2(0.0f) : readVec2(cube.find("uv"), glm::vec2(0.0f));
                const JsonValue* inf = cube.find("inflate");
                float inflate = inf && inf->isNumber() ? static_cast<float>(inf->number) : 0.0f;
                bool mirror = readBool(cube.find("mirror"), boneMirror);
                addBox(out.bones[idx], -(o.x + s.x), o.y, o.z, s.x, s.y, s.z, uv.x, uv.y,
                       inflate, mirror, perFace ? &pf : nullptr);
            }
        }
        return out;
    }
    std::fprintf(stderr, "[PlayerModel] %s not found in player.json\n", want);
    return out;
}

} // namespace mc
