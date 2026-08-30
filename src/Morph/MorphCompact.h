#pragma once

#include "../Math/HalfFloat.h"
#include "../Math/Vector3D.h"
#include "../Render/TriangleGroup.h"

// MorphCompact — a compact morph target for the JSON-loaded face
// (JsonNukudeFace). Indices stored as uint16, deltas as IEEE-754 half.
// 8 B/entry vs 16 B for Morph (int idx + Vector3D delta).
//
// The legacy embedded NukudeFace keeps using Morph (shared class); this type
// exists so JsonNukudeFace can compact storage WITHOUT touching the shared
// Morph class. Semantics match Morph::MorphObject3D exactly
// (vertex[idx] += delta * Weight), converting half->float at apply time.
class MorphCompact {
public:
    float Weight = 0.0f;

    MorphCompact(uint16_t count, uint16_t* indices, float16* deltas)
        : count(count), indices(indices), deltas(deltas) {}

    // Mirrors Morph::MorphObject3D. Weight gate is applied by the caller
    // (JsonNukudeFace::Update checks Weight > 0 before calling), same as Morph.
    void MorphObject3D(TriangleGroup* obj) {
        Vector3D* verts = obj->GetVertices();
        for (uint16_t i = 0; i < count; i++) {
            const size_t base = (size_t)i * 3;
            Vector3D d(HalfToFloat(deltas[base]),
                       HalfToFloat(deltas[base + 1]),
                       HalfToFloat(deltas[base + 2]));
            verts[indices[i]] = verts[indices[i]] + d * Weight;
        }
    }

private:
    uint16_t count = 0;
    uint16_t* indices;
    float16* deltas;
};
