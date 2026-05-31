#pragma once

#include <ArduinoJson.h>
#include <FS.h>
#include <memory>
#include <vector>
#include <string>
#include <ProtoGC.h>
#include "UniversalFace.h"
#include "Morph.h"
#include "../Materials/SimpleMaterial.h"
#include "../Render/IndexGroup.h"
#include "../Render/Object3D.h"

// Lightweight, runtime-loadable face model that mirrors the NukudeFace interface
// but sources its mesh and morph targets from a JSON file. This allows updating
// the face without recompiling firmware.
class JsonNukudeFace {
public:
    using Morphs = NukudeFace::Morphs;

    JsonNukudeFace() = default;
    ~JsonNukudeFace() = default;

    // Delete copy constructor and assignment operator (unique_ptr cannot be copied)
    JsonNukudeFace(const JsonNukudeFace&) = delete;
    JsonNukudeFace& operator=(const JsonNukudeFace&) = delete;

    // Default move constructor and assignment operator
    JsonNukudeFace(JsonNukudeFace&&) = default;
    JsonNukudeFace& operator=(JsonNukudeFace&&) = default;

    bool Load(fs::FS &fs, const char *path) {
        File file = fs.open(path, "r");
        if (!file) {
            Serial.println("[WARN] universal_face.json Failed to open file.");
            return false;
        }

        // Reset state so a failed load does not leave stale geometry.
        loaded = false;
        triangleGroup.reset();
        basisObj.reset();
        vertexBufferStorage.reset();
        indexBufferStorage.reset();
        morphs.clear();
        morphIndexStorage.clear();
        morphVectorStorage.clear();
        morphNames.clear();

        const size_t fileSize = file.size();
        // Allocate extra overhead for ArduinoJson metadata to avoid OOM on large (~200KB+) files.
        const size_t docCapacity = fileSize + 32768;

        Serial.printf("[INFO] universal_face.json size=%u bytes, docCapacity=%u\n", (unsigned)fileSize, (unsigned)docCapacity);

    #if defined(ESP32) && USE_PSRAM_FOR_FACE_JSON
        using FaceJsonDocument = BasicJsonDocument<protogc::ProtoJsonPsramAllocator>;
        FaceJsonDocument doc(docCapacity);
    #else
        DynamicJsonDocument doc(docCapacity);
    #endif

        if (doc.capacity() == 0)
        {
            Serial.println("[WARN] universal_face.json Failed to allocate JSON doc (capacity=0).");
            return false;
        }
        DeserializationError err = deserializeJson(doc, file);
        file.close();
        if (err) {
            Serial.printf("[WARN] universal_face.json Failed to parse JSON: %s (cap=%u, size=%u)\n", err.c_str(), (unsigned)docCapacity, (unsigned)fileSize);
            return false;
        }

        const uint16_t vertexCount = doc["vertexCount"].as<uint16_t>();
        const uint16_t triangleCount = doc["triangleCount"].as<uint16_t>();
        const uint8_t morphCount = doc["morphCount"].as<uint8_t>();

        JsonArray verticesArray = doc["vertices"].as<JsonArray>();
        JsonArray indicesArray = doc["triangles"].as<JsonArray>();
        JsonArray morphsArray = doc["morphs"].as<JsonArray>();

        if (!verticesArray || !indicesArray || !morphsArray) {
            Serial.printf("[WARN] universal_face.json Missing arrays (vertices=%d, indices=%d, morphs=%d)\n",
                          verticesArray.isNull(), indicesArray.isNull(), morphsArray.isNull());
            return false;
        }

        if (verticesArray.size() != static_cast<size_t>(vertexCount) * 3) {
            Serial.printf("[WARN] universal_face.json Vertex count mismatch: declared=%u actual=%u\n",
                          (unsigned)(vertexCount * 3), (unsigned)verticesArray.size());
            return false;
        }

        if (indicesArray.size() != static_cast<size_t>(triangleCount) * 3) {
            Serial.printf("[WARN] universal_face.json Index count mismatch: declared=%u actual=%u\n",
                          (unsigned)(triangleCount * 3), (unsigned)indicesArray.size());
            return false;
        }

        // Build vertex buffer (owned so transforms and morphs stay valid).
        vertexBufferStorage.reset(new Vector3D[vertexCount]);
        Vector3D *vertexBuffer = vertexBufferStorage.get();
        if (!vertexBuffer) {
            Serial.println("[WARN] universal_face.json Failed to alloc vertexBuffer");
            return false;
        }
        for (uint16_t i = 0; i < vertexCount; ++i) {
            const size_t base = static_cast<size_t>(i) * 3;
            vertexBuffer[i] = Vector3D(
                verticesArray[base].as<float>(),
                verticesArray[base + 1].as<float>(),
                verticesArray[base + 2].as<float>());
        }

        // Build index buffer (owned like the static model does).
        indexBufferStorage.reset(new IndexGroup[triangleCount]);
        IndexGroup *indexBuffer = indexBufferStorage.get();
        if (!indexBuffer) {
            Serial.println("[WARN] universal_face.json Failed to alloc indexBuffer");
            return false;
        }
        for (uint16_t i = 0; i < triangleCount; ++i) {
            const size_t base = static_cast<size_t>(i) * 3;
            indexBuffer[i] = IndexGroup(
                indicesArray[base].as<int>(),
                indicesArray[base + 1].as<int>(),
                indicesArray[base + 2].as<int>());
        }

        triangleGroup.reset(new TriangleGroup(vertexBuffer, indexBuffer, vertexCount, triangleCount));
        basisObj.reset(new Object3D(triangleGroup.get(), &simpleMaterial));

        // Build morph targets.
        morphs.clear();
        morphIndexStorage.clear();
        morphVectorStorage.clear();
        morphNames.clear();
        morphs.reserve(morphCount);
        morphIndexStorage.reserve(morphCount);
        morphVectorStorage.reserve(morphCount);

        for (JsonObject morphObj : morphsArray) {
            const uint16_t vCount = morphObj["vertexCount"].as<uint16_t>();
            JsonArray idxArray = morphObj["indices"].as<JsonArray>();
            JsonArray vecArray = morphObj["vectors"].as<JsonArray>();
            const char *morphName = morphObj["name"] | "";

            if (!idxArray || !vecArray || idxArray.size() != vCount || vecArray.size() != static_cast<size_t>(vCount) * 3) {
                Serial.printf("[WARN] universal_face.json Morph '%s' invalid sizes (idx=%u vs %u, vec=%u vs %u)\n",
                              morphName,
                              (unsigned)idxArray.size(), (unsigned)vCount,
                              (unsigned)vecArray.size(), (unsigned)(vCount * 3));
                return false;
            }

            std::unique_ptr<int[]> idxBuf(new int[vCount]);
            std::unique_ptr<Vector3D[]> vecBuf(new Vector3D[vCount]);

            for (uint16_t i = 0; i < vCount; ++i) {
                idxBuf[i] = idxArray[i].as<int>();
                const size_t base = static_cast<size_t>(i) * 3;
                vecBuf[i] = Vector3D(
                    vecArray[base].as<float>(),
                    vecArray[base + 1].as<float>(),
                    vecArray[base + 2].as<float>());
            }

            morphIndexStorage.push_back(std::move(idxBuf));
            morphVectorStorage.push_back(std::move(vecBuf));
            morphs.emplace_back(vCount, morphIndexStorage.back().get(), morphVectorStorage.back().get());
            morphNames.emplace_back(morphName);
        }

        Serial.printf("[INFO] universal_face.json Loaded successfully: vertices=%u, triangles=%u, morphs=%u\n",
                      (unsigned)vertexCount, (unsigned)triangleCount, (unsigned)morphCount);

        loaded = true;
        return true;
    }

    // Compatibility helpers to mirror UniversalFace accessors.
    TriangleGroup *GetTriangleGroup() {
        return triangleGroup.get();
    }

    Transform *GetTransform() {
        return basisObj ? basisObj->GetTransform() : nullptr;
    }

    Object3D *GetObject() {
        return basisObj.get();
    }

    void SetMorphWeight(Morphs morph, float weight) {
        if (loaded && morph < morphs.size()) {
            morphs[static_cast<size_t>(morph)].Weight = weight;
        }
    }

    // Convenience: set morph weight by name (case-sensitive). Returns true on success.
    bool SetMorphWeightByName(const std::string &name, float weight) {
        int idx = FindMorphIndexByName(name);
        if (idx >= 0) {
            morphs[static_cast<size_t>(idx)].Weight = weight;
            return true;
        }
        return false;
    }

    // Returns index or -1 if not found.
    int FindMorphIndexByName(const std::string &name) const {
        if (!loaded) {
            return -1;
        }
        for (size_t i = 0; i < morphNames.size(); ++i) {
            if (morphNames[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    float *GetMorphWeightReference(Morphs morph) {
        if (loaded && morph < morphs.size()) {
            return &morphs[static_cast<size_t>(morph)].Weight;
        }
        return nullptr;
    }

    void Reset() {
        if (!loaded) {
            return;
        }
        for (auto &m : morphs) {
            m.Weight = 0.0f;
        }
    }

    void Update() {
        if (!loaded || !basisObj) {
            return;
        }

        basisObj->ResetVertices();
        for (auto &m : morphs) {
            if (m.Weight > 0.0f) {
                m.MorphObject3D(basisObj->GetTriangleGroup());
            }
        }
    }

    bool Loaded() const {
        return loaded;
    }

    float *GetMorphWeightReferenceByName(const std::string &name) {
        int idx = FindMorphIndexByName(name);
        if (idx >= 0) {
            return &morphs[static_cast<size_t>(idx)].Weight;
        }
        return nullptr;
    }

    // Expose morph metadata for dynamic registration.
    const std::vector<std::string> &GetMorphNames() const {
        return morphNames;
    }

    size_t GetMorphCount() const {
        return morphNames.size();
    }

    float *GetMorphWeightReferenceByIndex(size_t idx) {
        if (loaded && idx < morphs.size()) {
            return &morphs[idx].Weight;
        }
        return nullptr;
    }

private:
    bool loaded = false;
    SimpleMaterial simpleMaterial = SimpleMaterial(ProtoRGBColor(128, 128, 128));
    std::unique_ptr<Vector3D[]> vertexBufferStorage;
    std::unique_ptr<IndexGroup[]> indexBufferStorage;
    std::unique_ptr<TriangleGroup> triangleGroup;
    std::unique_ptr<Object3D> basisObj;
    std::vector<Morph> morphs;
    std::vector<std::unique_ptr<int[]>> morphIndexStorage;
    std::vector<std::unique_ptr<Vector3D[]>> morphVectorStorage;
    std::vector<std::string> morphNames;
};
