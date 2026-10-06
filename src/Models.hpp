#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Static model geometry for 3.3.5: M2 doodads (version 264, with Name00.skin) and WMO v17 buildings.
// Parsed into one plain mesh per model, already in editor axes (y up). No GPU types here.

struct ModelVertex
{
    DirectX::XMFLOAT3 pos; DirectX::XMFLOAT3 nrm; DirectX::XMFLOAT2 uv;
    uint8_t bones[4] = { 0, 0, 0, 0 }, weights[4] = { 255, 0, 0, 0 };   // M2 bone indices and weights (sum 255)
};

/// One animated value of a bone for the chosen animation (or its global sequence).
template <class T> struct ModelTrack
{
    int16_t globalSequence = -1;
    bool linear = true;               // false: hold each key (interpolation 0)
    std::vector<uint32_t> times;      // ms
    std::vector<T> values;
};

/// One entry of an M2's animation list (M2Sequence).
struct ModelSequence
{
    uint16_t id = 0, variation = 0;   // AnimationData id (0 Stand, 4 Walk, ...) and which variant of it
    uint32_t duration = 0;            // ms
    uint32_t flags = 0;               // 0x20 kept in the .m2 (else in a .anim file), 0x40 alias of `alias`
    uint16_t alias = 0;               // the sequence this one plays instead (flag 0x40)
};

/// The bones of an M2 with one animation (Stand unless asked for another) and the global sequences, enough to pose
/// every frame.
struct ModelSkeleton
{
    struct Bone
    {
        int16_t parent = -1;
        DirectX::XMFLOAT3 pivot{};   // WoW axes
        ModelTrack<DirectX::XMFLOAT3> translation, scale;
        ModelTrack<DirectX::XMFLOAT4> rotation;   // quaternion x, y, z, w
    };
    struct Attachment { uint32_t id; uint16_t bone; DirectX::XMFLOAT3 pos; };   // pos: WoW axes
    std::vector<Bone> bones;
    std::vector<uint32_t> globalSequences;   // ms
    uint32_t duration = 0;                   // the animation, ms
    std::vector<Attachment> attachments;
    /// Texture animation (same animation / global sequences as the bones): UV transforms, transparency weights, and
    /// colour alpha; batches point into them (ModelMesh::Batch uvAnim / weight / color).
    struct UvAnim { ModelTrack<DirectX::XMFLOAT3> translation, scale; };   // ponytail: UV rotation not read (rare)
    std::vector<UvAnim> uvAnims;
    std::vector<ModelTrack<float>> weights, colorAlphas;
    bool animated = false;                   // some track has keys
    std::vector<ModelSequence> sequences;    // every animation of the model, in file order
    int sequence = -1;                       // the one loaded (index into sequences), -1 none
};
/// The bone palette at `timeMs` in editor axes (row vectors: skinned = vertex * palette[bone]).
void PoseBones(const ModelSkeleton& skeleton, uint32_t timeMs, std::vector<DirectX::XMFLOAT4X4>& palette);
/// A batch's texture transform at timeMs as uv' = (uv - 0.5) * (x, y) + 0.5 + (z, w); identity for index -1.
DirectX::XMFLOAT4 UvTransform(const ModelSkeleton& skeleton, int uvAnim, uint32_t timeMs);
/// A batch's opacity at timeMs: its transparency weight times its colour's alpha (1 when it has neither).
float BatchAlpha(const ModelSkeleton& skeleton, int weight, int color, uint32_t timeMs);

struct ModelMesh
{
    enum class Blend { Opaque, AlphaTest, AlphaBlend, Additive };
    struct Batch
    {
        uint32_t indexStart = 0, indexCount = 0;
        std::string texture;   // empty = no fixed texture (replaceable skins, missing names)
        uint32_t textureType = 0;   // M2 texture type: 0 fixed (texture), else replaceable (1 body, 2 cape, 6 hair, 8 fur, 11-13 creature skins)
        uint16_t geoset = 0;        // M2 submesh id (group * 100 + variant); WMO 0
        uint16_t liquid = 0;        // WMO liquid: its LiquidType id (animated frames, drawn as water); 0 = not a liquid
        int16_t uvAnim = -1, weight = -1, color = -1;   // M2: texture transform, transparency weight, colour (ModelSkeleton), -1 = none
        Blend blend = Blend::Opaque;
        bool twoSided = false;
    };
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    DirectX::XMFLOAT3 boundsMin{}, boundsMax{};   // model space, editor axes
    std::shared_ptr<const ModelSkeleton> skeleton;   // M2 with bones; null for WMOs
    /// WMO doodads (WoW axes, relative to the WMO) and the sets choosing among them: set 0 always, plus the
    /// placement's doodadSet.
    struct Doodad { std::string model; DirectX::XMFLOAT3 pos{}; DirectX::XMFLOAT4 rot{ 0, 0, 0, 1 }; float scale = 1; };
    struct DoodadSet { uint32_t first = 0, count = 0; };
    std::vector<Doodad> doodads;
    std::vector<DoodadSet> doodadSets;
};

/// WoW model files are z-up; the editor is y-up (x, y, z) -> (x, z, -y).
inline DirectX::XMFLOAT3 FromWowAxes(float x, float y, float z) { return { x, z, -y }; }

/// Placement matrix for MDDF/MODF entries: rotation in degrees as stored, uniform scale.
DirectX::XMMATRIX PlacementMatrix(const float pos[3], const float rotDegrees[3], float scale);
/// A WMO doodad's matrix inside its WMO, in editor axes (multiply by the WMO's placement for the world).
DirectX::XMMATRIX WmoDoodadMatrix(const ModelMesh::Doodad& d);
/// The inverse: position, stored rotation (degrees) and uniform scale (row length average) of a placement matrix.
void DecomposePlacement(DirectX::FXMMATRIX m, float pos[3], float rotDegrees[3], float& scale);

/// Reads a file from the client (null when missing).
using FileReader = std::function<std::optional<std::vector<uint8_t>>(const std::string& path)>;

/// `anim` reads the .anim file of a sequence kept outside the .m2 (may be empty: such animations stay still).
std::optional<ModelMesh> ParseM2(const std::vector<uint8_t>& m2, const std::vector<uint8_t>& skin, const FileReader& anim = {}, const std::string& m2Name = {});
/// An M2 by name with its skin and the .anim file it needs.
std::optional<ModelMesh> LoadM2(const std::string& m2Name, const FileReader& read);
/// The skeleton of an M2 posed by animation `sequence` (index into its sequence list; aliases followed), reading the
/// .anim file it needs; null when the model has no bones.
std::shared_ptr<const ModelSkeleton> LoadSkeleton(const std::string& m2Name, const FileReader& read, int sequence);
/// `groups` holds Root_000.wmo ... in order; missing groups may be empty vectors.
std::optional<ModelMesh> ParseWmo(const std::vector<uint8_t>& root, const std::vector<std::vector<uint8_t>>& groups);
/// Number of groups and the root bounding box (WoW axes, min then max) from a WMO root.
bool WmoRootInfo(const std::vector<uint8_t>& root, uint32_t& groups, float bounds[6]);
/// What WMOAreaTable keys a WMO by: the root's WMOID (MOHD) and each group's id (MOGP) with its name (MOGN).
struct WmoAreaKeys { uint32_t wmoId = 0; std::vector<std::pair<uint32_t, std::string>> groups; };
/// `read` gets a file by name; missing groups are skipped. None when the root has no MOHD.
std::optional<WmoAreaKeys> ReadWmoAreaKeys(const std::string& rootName, const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read);

/// "World\\Foo\\Bar.mdx" -> "World\\Foo\\Bar.m2"; the skin is "World\\Foo\\Bar00.skin".
std::string M2Name(const std::string& placementName);
std::string M2SkinName(const std::string& m2Name);
std::string WmoGroupName(const std::string& rootName, uint32_t group);
