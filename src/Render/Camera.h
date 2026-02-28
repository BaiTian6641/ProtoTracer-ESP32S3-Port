#pragma once

#include "../Math/Rotation.h"
#include "../Math/Transform.h"
#include "../Render/CameraLayout.h"
#include "CameraBase.h"
#include "PixelGroup.h"
#include "Scene.h"
#include "Triangle2D.h"
#include "QuadTree.h"
#include "Node.h"
#include <esp_heap_caps.h>
#include <esp_dsp.h>
#include <cstdint>
#include <cstring>

#ifndef RENDER_SIMD_CAMERA_RAYS
#define RENDER_SIMD_CAMERA_RAYS 1
#endif

#ifndef RENDER_SIMD_CAMERA_OBJECT_PROJ
#define RENDER_SIMD_CAMERA_OBJECT_PROJ 1
#endif

#ifndef RENDER_CAMERA_TREE_CACHE
#define RENDER_CAMERA_TREE_CACHE 1
#endif

#ifndef RENDER_PREFER_PSRAM_BUFFERS
#define RENDER_PREFER_PSRAM_BUFFERS 1
#endif

//template<size_t pixelCount>
class Camera : public CameraBase{
private:
    Transform* transform;
    CameraLayout* cameraLayout;
    // --- MODIFIED: Pointer is to the non-templated PixelGroup class ---
    PixelGroup* pixelGroup; 
    Quaternion rayDirection;
    Quaternion invRayDirection;
    Quaternion lookDirection;
    Quaternion lookOffset;
    bool is2D = false;

    Vector2D* cachedRays = nullptr; // reused buffer to avoid per-frame allocations
    unsigned int cachedRayCount = 0;
    unsigned int cachedFloatCount = 0;

    // SIMD buffers for batch rotate/scale
    float* tmpX = nullptr;
    float* tmpY = nullptr;
    float* rotX = nullptr;
    float* rotY = nullptr;
    bool tmpXHeapCapsOwned = false;
    bool tmpYHeapCapsOwned = false;
    bool rotXHeapCapsOwned = false;
    bool rotYHeapCapsOwned = false;

    // Object projection scratch (vertex-space SoA, reused per object)
    float* objX = nullptr;
    float* objY = nullptr;
    float* objZ = nullptr;
    float* projX = nullptr;
    float* projY = nullptr;
    float* projZ = nullptr;
    unsigned int cachedObjectVertexCount = 0;
    bool objXHeapCapsOwned = false;
    bool objYHeapCapsOwned = false;
    bool objZHeapCapsOwned = false;
    bool projXHeapCapsOwned = false;
    bool projYHeapCapsOwned = false;
    bool projZHeapCapsOwned = false;

    void FreeFloatBuffer(float*& buffer, bool& heapCapsOwned) {
        if (!buffer) return;

        if (heapCapsOwned) {
            heap_caps_free(buffer);
        } else {
            delete[] buffer;
        }

        buffer = nullptr;
        heapCapsOwned = false;
    }

    struct Rotation2D {
        float m00;
        float m01;
        float m10;
        float m11;
    };

    Triangle2D::Rotation3DMatrix invRayRotation;
    QuadTree* cachedTree = nullptr;
    BoundingBox2D cachedBounds;
    uint32_t cachedCameraSignature = 0;
    uint32_t cachedSceneSignature = 0;
    bool cachedRaysValid = false;
    bool cachedTreeValid = false;

    static inline uint32_t HashMix(uint32_t hash, uint32_t value) {
        hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        return hash;
    }

    static inline uint32_t FloatBits(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(float));
        return bits;
    }

    static inline uint32_t PtrBits(const void* ptr) {
        const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
#if UINTPTR_MAX > 0xFFFFFFFFu
        return static_cast<uint32_t>(value ^ (value >> 32));
#else
        return static_cast<uint32_t>(value);
#endif
    }

    static inline uint32_t HashVector3(uint32_t hash, const Vector3D& v) {
        hash = HashMix(hash, FloatBits(v.X));
        hash = HashMix(hash, FloatBits(v.Y));
        hash = HashMix(hash, FloatBits(v.Z));
        return hash;
    }

    static inline uint32_t HashQuaternion(uint32_t hash, const Quaternion& q) {
        hash = HashMix(hash, FloatBits(q.W));
        hash = HashMix(hash, FloatBits(q.X));
        hash = HashMix(hash, FloatBits(q.Y));
        hash = HashMix(hash, FloatBits(q.Z));
        return hash;
    }

    uint32_t BuildCameraSignature(const Vector2D* coordinateArray,
                                  bool reverseCoordinateArray,
                                  unsigned int pixelCount,
                                  const Vector3D& camPos,
                                  const Vector3D& scale,
                                  const Quaternion& camRot,
                                  const Quaternion& normLookDir) const {
        uint32_t hash = 2166136261u;
        hash = HashMix(hash, PtrBits(pixelGroup));
        hash = HashMix(hash, PtrBits(coordinateArray));
        hash = HashMix(hash, reverseCoordinateArray ? 1u : 0u);
        hash = HashMix(hash, pixelCount);
        hash = HashVector3(hash, camPos);
        hash = HashVector3(hash, scale);
        hash = HashQuaternion(hash, camRot);
        hash = HashQuaternion(hash, normLookDir);
        hash = HashQuaternion(hash, lookOffset);
        return hash;
    }

    uint32_t BuildSceneSignature(Object3D** sceneObjects, unsigned int sceneObjectCount) const {
        uint32_t hash = 2166136261u;
        hash = HashMix(hash, sceneObjectCount);

        for (unsigned int i = 0; i < sceneObjectCount; ++i) {
            Object3D* object = sceneObjects[i];
            hash = HashMix(hash, PtrBits(object));

            if (!object) {
                continue;
            }

            const bool enabled = object->IsEnabled();
            hash = HashMix(hash, enabled ? 1u : 0u);
            hash = HashMix(hash, PtrBits(object->GetMaterial()));

            TriangleGroup* triangleGroup = object->GetTriangleGroup();
            hash = HashMix(hash, PtrBits(triangleGroup));

            Transform* objTransform = object->GetTransform();
            if (objTransform) {
                hash = HashVector3(hash, objTransform->GetPosition());
                hash = HashVector3(hash, objTransform->GetScale());
                hash = HashQuaternion(hash, objTransform->GetRotation());
            }

            if (!triangleGroup) {
                continue;
            }

            const int vertexCount = triangleGroup->GetVertexCount();
            const int triangleCount = triangleGroup->GetTriangleCount();
            hash = HashMix(hash, static_cast<uint32_t>(vertexCount));
            hash = HashMix(hash, static_cast<uint32_t>(triangleCount));

            if (vertexCount > 0) {
                Vector3D* vertices = triangleGroup->GetVertices();
                if (vertices) {
                    const int i0 = 0;
                    const int i1 = vertexCount / 2;
                    const int i2 = vertexCount - 1;
                    hash = HashVector3(hash, vertices[i0]);
                    hash = HashVector3(hash, vertices[i1]);
                    hash = HashVector3(hash, vertices[i2]);
                }
            }
        }

        return hash;
    }

    void EnsureRayCache() {
        const unsigned int desired = pixelGroup ? pixelGroup->GetPixelCount() : 0;
        if (desired == 0) return;

        if (cachedRayCount != desired || cachedRays == nullptr) {
            delete[] cachedRays;
            cachedRays = new Vector2D[desired]; // small; keep on internal heap by default
            cachedRayCount = desired;
        }
    }

    void EnsureFloatCache() {
        const unsigned int desired = pixelGroup ? pixelGroup->GetPixelCount() : 0;
        if (desired == 0) return;

        if (cachedFloatCount != desired || tmpX == nullptr || tmpY == nullptr || rotX == nullptr || rotY == nullptr) {
            FreeFloatBuffer(tmpX, tmpXHeapCapsOwned);
            FreeFloatBuffer(tmpY, tmpYHeapCapsOwned);
            FreeFloatBuffer(rotX, rotXHeapCapsOwned);
            FreeFloatBuffer(rotY, rotYHeapCapsOwned);

#if RENDER_PREFER_PSRAM_BUFFERS
            tmpX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            tmpY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            rotX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            rotY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

            if (!tmpX) tmpX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!tmpY) tmpY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!rotX) rotX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!rotY) rotY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

            if (!tmpX) tmpX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!tmpY) tmpY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!rotX) rotX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!rotY) rotY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
#else
            tmpX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            tmpY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            rotX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            rotY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
#endif

            if (!tmpX) tmpX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!tmpY) tmpY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!rotX) rotX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!rotY) rotY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            tmpXHeapCapsOwned = (tmpX != nullptr);
            tmpYHeapCapsOwned = (tmpY != nullptr);
            rotXHeapCapsOwned = (rotX != nullptr);
            rotYHeapCapsOwned = (rotY != nullptr);

            // Fallback to heap if internal allocation fails
            if (!tmpX) {
                tmpX = new float[desired];
                tmpXHeapCapsOwned = false;
            }
            if (!tmpY) {
                tmpY = new float[desired];
                tmpYHeapCapsOwned = false;
            }
            if (!rotX) {
                rotX = new float[desired];
                rotXHeapCapsOwned = false;
            }
            if (!rotY) {
                rotY = new float[desired];
                rotYHeapCapsOwned = false;
            }

            cachedFloatCount = desired;
        }
    }

    void EnsureObjectProjectionCache(unsigned int desired) {
        if (desired == 0) return;

        if (cachedObjectVertexCount != desired || objX == nullptr || objY == nullptr || objZ == nullptr || projX == nullptr || projY == nullptr || projZ == nullptr) {
            FreeFloatBuffer(objX, objXHeapCapsOwned);
            FreeFloatBuffer(objY, objYHeapCapsOwned);
            FreeFloatBuffer(objZ, objZHeapCapsOwned);
            FreeFloatBuffer(projX, projXHeapCapsOwned);
            FreeFloatBuffer(projY, projYHeapCapsOwned);
            FreeFloatBuffer(projZ, projZHeapCapsOwned);

#if RENDER_PREFER_PSRAM_BUFFERS
            objX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            objY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            objZ = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            projX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            projY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            projZ = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

            if (!objX) objX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!objY) objY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!objZ) objZ = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!projX) projX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!projY) projY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!projZ) projZ = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

            if (!objX) objX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!objY) objY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!objZ) objZ = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!projX) projX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!projY) projY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!projZ) projZ = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
#else
            objX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            objY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            objZ = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            projX = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            projY = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            projZ = static_cast<float*>(heap_caps_aligned_alloc(16, desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
#endif

            if (!objX) objX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!objY) objY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!objZ) objZ = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!projX) projX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!projY) projY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            if (!projZ) projZ = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

            objXHeapCapsOwned = (objX != nullptr);
            objYHeapCapsOwned = (objY != nullptr);
            objZHeapCapsOwned = (objZ != nullptr);
            projXHeapCapsOwned = (projX != nullptr);
            projYHeapCapsOwned = (projY != nullptr);
            projZHeapCapsOwned = (projZ != nullptr);

            if (!objX) { objX = new float[desired]; objXHeapCapsOwned = false; }
            if (!objY) { objY = new float[desired]; objYHeapCapsOwned = false; }
            if (!objZ) { objZ = new float[desired]; objZHeapCapsOwned = false; }
            if (!projX) { projX = new float[desired]; projXHeapCapsOwned = false; }
            if (!projY) { projY = new float[desired]; projYHeapCapsOwned = false; }
            if (!projZ) { projZ = new float[desired]; projZHeapCapsOwned = false; }

            cachedObjectVertexCount = desired;
        }
    }

    Rotation2D BuildRotation2D(const Quaternion& qUnit) {
        // Derive a 2x2 rotation matrix from a unit quaternion (xy terms only)
        const float w = qUnit.W;
        const float x = qUnit.X;
        const float y = qUnit.Y;
        const float z = qUnit.Z;

        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float wz = w * z;

        Rotation2D m;
        m.m00 = 1.0f - 2.0f * (yy + zz);
        m.m01 = 2.0f * (xy - wz);
        m.m10 = 2.0f * (xy + wz);
        m.m11 = 1.0f - 2.0f * (xx + zz);
        return m;
    }

    Triangle2D::Rotation3DMatrix BuildRotation3D(const Quaternion& qUnit) {
        const float w = qUnit.W;
        const float x = qUnit.X;
        const float y = qUnit.Y;
        const float z = qUnit.Z;

        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float wx = w * x;
        const float wy = w * y;
        const float wz = w * z;

        Triangle2D::Rotation3DMatrix m;
        m.m00 = 1.0f - 2.0f * (yy + zz);
        m.m01 = 2.0f * (xy - wz);
        m.m02 = 2.0f * (xz + wy);

        m.m10 = 2.0f * (xy + wz);
        m.m11 = 1.0f - 2.0f * (xx + zz);
        m.m12 = 2.0f * (yz - wx);

        m.m20 = 2.0f * (xz - wy);
        m.m21 = 2.0f * (yz + wx);
        m.m22 = 1.0f - 2.0f * (xx + yy);

        return m;
    }

    ProtoRGBColor CheckRasterPixel(Triangle2D** triangles, int numTriangles, Vector2D pixelRay){
        float zBuffer = 3.402823466e+38f;
        int triangle = 0;
        bool didIntersect = false;
        float u = 0.0f, v = 0.0f, w = 0.0f;
        Vector3D uvw;
        ProtoRGBColor color;
        
        for (int t = 0; t < numTriangles; t++) {
            if (triangles[t]->averageDepth < zBuffer){
                if(triangles[t]->DidIntersect(pixelRay.X, pixelRay.Y, u, v, w)){
                    uvw.X = u;
                    uvw.Y = v;
                    uvw.Z = w;
                    zBuffer = triangles[t]->averageDepth;
                    triangle = t;
                    didIntersect = true;
                }
            }
        }

        if(didIntersect){
            Vector3D intersect = (*triangles[triangle]->t3p1 * uvw.X) + (*triangles[triangle]->t3p2 * uvw.Y) + (*triangles[triangle]->t3p3 * uvw.Z);
            const float ix = intersect.X;
            const float iy = intersect.Y;
            const float iz = intersect.Z;

            intersect.X = invRayRotation.m00 * ix + invRayRotation.m01 * iy + invRayRotation.m02 * iz;
            intersect.Y = invRayRotation.m10 * ix + invRayRotation.m11 * iy + invRayRotation.m12 * iz;
            intersect.Z = invRayRotation.m20 * ix + invRayRotation.m21 * iy + invRayRotation.m22 * iz;
            Vector2D uv;

            if (triangles[triangle]->hasUV){
                uv = *triangles[triangle]->p1UV * uvw.X + *triangles[triangle]->p2UV * uvw.Y + *triangles[triangle]->p3UV * uvw.Z;
            }
            
            color = triangles[triangle]->GetMaterial()->GetRGB(intersect, *triangles[triangle]->normal, Vector3D(uv.X, uv.Y, 0.0f));
        }
        
        return color;
    }

public:
    Camera(Transform* transform, PixelGroup* pixelGroup) {
        this->transform = transform;
        this->pixelGroup = pixelGroup;

        is2D = true;
    }

    Camera(Transform* transform, CameraLayout* cameraLayout, PixelGroup* pixelGroup) {
        this->transform = transform;
        this->pixelGroup = pixelGroup;
        this->cameraLayout = cameraLayout;

        transform->SetBaseRotation(cameraLayout->GetRotation());
    }

    ~Camera() {
        delete cachedTree;
        delete[] cachedRays;
        FreeFloatBuffer(tmpX, tmpXHeapCapsOwned);
        FreeFloatBuffer(tmpY, tmpYHeapCapsOwned);
        FreeFloatBuffer(rotX, rotXHeapCapsOwned);
        FreeFloatBuffer(rotY, rotYHeapCapsOwned);
        FreeFloatBuffer(objX, objXHeapCapsOwned);
        FreeFloatBuffer(objY, objYHeapCapsOwned);
        FreeFloatBuffer(objZ, objZHeapCapsOwned);
        FreeFloatBuffer(projX, projXHeapCapsOwned);
        FreeFloatBuffer(projY, projYHeapCapsOwned);
        FreeFloatBuffer(projZ, projZHeapCapsOwned);
    }

    Transform* GetTransform(){
        return transform;
    }

    PixelGroup* GetPixelGroup(){
        return pixelGroup;
    }

    CameraLayout* GetCameraLayout(){
        return cameraLayout;
    }

    // --- OPTIMIZATION: Replaced inefficient loops with a direct call to PixelGroup's bounds ---
    // This assumes you add a GetBounds() method to PixelGroup as shown in the next section.
    Vector2D GetCameraMinCoordinate() {
        // O(1) instead of O(N)
        return pixelGroup->GetBounds().GetMinimum();
    }

    Vector2D GetCameraMaxCoordinate() {
        // O(1) instead of O(N)
        return pixelGroup->GetBounds().GetMaximum();
    }

    Vector2D GetCameraCenterCoordinate() {
        // O(1) instead of O(N)
        return pixelGroup->GetBounds().GetCenter();
    }

    void SetLookOffset(Quaternion lookOffset) {
        this->lookOffset = lookOffset;
        cachedRaysValid = false;
        cachedTreeValid = false;
    }

    void Rasterize(Scene* scene) override {
        Object3D** sceneObjects = scene->GetCachedObjects();
        const unsigned int sceneObjectCount = scene->GetCachedObjectCount();
        const Vector2D* coordinateArray = pixelGroup->GetCoordinateArray();
        const bool reverseCoordinateArray = pixelGroup->IsCoordinateArrayReversed();
        const unsigned int pixelCount = pixelGroup->GetPixelCount();

        if (is2D){
            for (unsigned int i = 0; i < pixelCount; i++) {
                Vector2D pixelRay;
                if (coordinateArray) {
                    pixelRay = reverseCoordinateArray ? coordinateArray[pixelCount - i - 1] : coordinateArray[i];
                } else {
                    pixelRay = pixelGroup->GetCoordinate(i);
                }

                Vector3D pixelRay3D = Vector3D(pixelRay.X, pixelRay.Y, 0) + transform->GetPosition();

                ProtoRGBColor color = sceneObjects[0]->GetMaterial()->GetRGB(pixelRay3D, Vector3D(), Vector3D());

                pixelGroup->GetColor(i)->R = color.R;
                pixelGroup->GetColor(i)->G = color.G;
                pixelGroup->GetColor(i)->B = color.B;
            }
        }
        else{
            lookDirection = transform->GetRotation().Conjugate() * lookOffset;
            Quaternion normLookDir = lookDirection.UnitQuaternion();
            Quaternion camRot = transform->GetRotation();
            rayDirection  = camRot.Multiply(lookDirection);
            invRayDirection = rayDirection.UnitQuaternion().Conjugate();
            invRayRotation = BuildRotation3D(invRayDirection);
            const Vector3D scale = transform->GetScale();
            const Rotation2D rot2 = BuildRotation2D(normLookDir);
            const Vector3D camPos = transform->GetPosition();
            const Triangle2D::Rotation3DMatrix invViewRotation = invRayRotation;

#if RENDER_CAMERA_TREE_CACHE
            const uint32_t cameraSignature = BuildCameraSignature(coordinateArray, reverseCoordinateArray, pixelCount, camPos, scale, camRot, normLookDir);
            const bool cameraChanged = !cachedRaysValid || (cameraSignature != cachedCameraSignature);
#else
            const bool cameraChanged = true;
#endif

            BoundingBox2D transformedBounds;

            if (cameraChanged) {
                EnsureRayCache();
                EnsureFloatCache();

                // Batch scale then rotate using ESP-DSP vector ops
                for (unsigned int i = 0; i < pixelCount; ++i) {
                    const Vector2D baseRay = coordinateArray
                        ? (reverseCoordinateArray ? coordinateArray[pixelCount - i - 1] : coordinateArray[i])
                        : pixelGroup->GetCoordinate(i);
                    tmpX[i] = baseRay.X * scale.X;
                    tmpY[i] = baseRay.Y * scale.Y;
                }

#if RENDER_SIMD_CAMERA_RAYS
                dsps_mulc_f32(tmpX, rotX, pixelCount, rot2.m00, 1, 1);
                dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m10, 1, 1);
                dsps_add_f32(rotX, rotY, rotX, pixelCount, 1, 1, 1); // rotX = m00*x + m10*y

                dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m11, 1, 1); // reuse rotY buffer
                dsps_mulc_f32(tmpX, tmpX, pixelCount, rot2.m01, 1, 1); // tmpX now holds m01*x
                dsps_add_f32(rotY, tmpX, rotY, pixelCount, 1, 1, 1); // rotY = m11*y + m01*x
#else
                for (unsigned int i = 0; i < pixelCount; ++i) {
                    const float x = tmpX[i];
                    const float y = tmpY[i];
                    rotX[i] = rot2.m00 * x + rot2.m10 * y;
                    rotY[i] = rot2.m01 * x + rot2.m11 * y;
                }
#endif

                for (unsigned int i = 0; i < pixelCount; ++i) {
                    Vector2D rotatedRay(rotX[i], rotY[i]);
                    cachedRays[i] = rotatedRay;
                    transformedBounds.UpdateBounds(rotatedRay);
                }

#if RENDER_CAMERA_TREE_CACHE
                cachedBounds = transformedBounds;
                cachedCameraSignature = cameraSignature;
                cachedRaysValid = true;
#endif
            } else {
                transformedBounds = cachedBounds;
            }

#if RENDER_CAMERA_TREE_CACHE
            const uint32_t sceneSignature = BuildSceneSignature(sceneObjects, sceneObjectCount);
            const bool rebuildTree = !cachedTreeValid || !cachedTree || cameraChanged || (sceneSignature != cachedSceneSignature);

            if (rebuildTree) {
                int reserveTriangles = 0;
                for (unsigned int i = 0; i < sceneObjectCount; i++) {
                    if (sceneObjects[i]->IsEnabled()) {
                        reserveTriangles += sceneObjects[i]->GetTriangleGroup()->GetTriangleCount();
                    }
                }

                delete cachedTree;
                cachedTree = new QuadTree(transformedBounds, reserveTriangles);

                //for each object in the scene, get the triangles
                for(unsigned int i = 0; i < sceneObjectCount; i++){
                    if(sceneObjects[i]->IsEnabled()){
                        //for each triangle in object, project onto 2d surface, but pass material
                        TriangleGroup* triangleGroup = sceneObjects[i]->GetTriangleGroup();
                        Triangle3D* triangles = triangleGroup->GetTriangles();
                        IndexGroup* indexGroup = triangleGroup->GetIndexGroup();
                        Vector3D* vertices = triangleGroup->GetVertices();
                        const int vertexCount = triangleGroup->GetVertexCount();
                        const int triangleCount = triangleGroup->GetTriangleCount();
                        Material* material = sceneObjects[i]->GetMaterial();

#if RENDER_SIMD_CAMERA_OBJECT_PROJ
                        if (vertexCount > 0) {
                            EnsureObjectProjectionCache(static_cast<unsigned int>(vertexCount));

                            for (int v = 0; v < vertexCount; ++v) {
                                objX[v] = vertices[v].X - camPos.X;
                                objY[v] = vertices[v].Y - camPos.Y;
                                objZ[v] = vertices[v].Z - camPos.Z;
                            }

                            dsps_mulc_f32(objX, projX, vertexCount, invViewRotation.m00, 1, 1);
                            dsps_mulc_f32(objY, projY, vertexCount, invViewRotation.m01, 1, 1);
                            dsps_add_f32(projX, projY, projX, vertexCount, 1, 1, 1);
                            dsps_mulc_f32(objZ, projY, vertexCount, invViewRotation.m02, 1, 1);
                            dsps_add_f32(projX, projY, projX, vertexCount, 1, 1, 1);

                            dsps_mulc_f32(objX, projY, vertexCount, invViewRotation.m10, 1, 1);
                            dsps_mulc_f32(objY, projZ, vertexCount, invViewRotation.m11, 1, 1);
                            dsps_add_f32(projY, projZ, projY, vertexCount, 1, 1, 1);
                            dsps_mulc_f32(objZ, projZ, vertexCount, invViewRotation.m12, 1, 1);
                            dsps_add_f32(projY, projZ, projY, vertexCount, 1, 1, 1);

                            dsps_mulc_f32(objX, projZ, vertexCount, invViewRotation.m20, 1, 1);
                            dsps_mulc_f32(objY, objX, vertexCount, invViewRotation.m21, 1, 1);
                            dsps_add_f32(projZ, objX, projZ, vertexCount, 1, 1, 1);
                            dsps_mulc_f32(objZ, objX, vertexCount, invViewRotation.m22, 1, 1);
                            dsps_add_f32(projZ, objX, projZ, vertexCount, 1, 1, 1);
                        }

                        for (int j = 0; j < triangleCount; j++) {
                            const int a = indexGroup[j].A;
                            const int b = indexGroup[j].B;
                            const int c = indexGroup[j].C;

                            cachedTree->Insert(Triangle2D(
                                Vector3D(projX[a], projY[a], projZ[a]),
                                Vector3D(projX[b], projY[b], projZ[b]),
                                Vector3D(projX[c], projY[c], projZ[c]),
                                &triangles[j],
                                material
                            ));
                        }
#else
                        for (int j = 0; j < triangleCount; j++) {
                            cachedTree->Insert(Triangle2D(invViewRotation, camPos, &triangles[j], material));
                        }
#endif
                    }
                }

                cachedTree->Rebuild();
                cachedTreeValid = true;
                cachedSceneSignature = sceneSignature;
                cachedCameraSignature = cameraSignature;
            }

#else
            int reserveTriangles = 0;
            for (unsigned int i = 0; i < sceneObjectCount; i++) {
                if (sceneObjects[i]->IsEnabled()) {
                    reserveTriangles += sceneObjects[i]->GetTriangleGroup()->GetTriangleCount();
                }
            }

            QuadTree frameTree(transformedBounds, reserveTriangles);

            //for each object in the scene, get the triangles
            for(unsigned int i = 0; i < sceneObjectCount; i++){
                if(sceneObjects[i]->IsEnabled()){
                    //for each triangle in object, project onto 2d surface, but pass material
                    TriangleGroup* triangleGroup = sceneObjects[i]->GetTriangleGroup();
                    Triangle3D* triangles = triangleGroup->GetTriangles();
                    IndexGroup* indexGroup = triangleGroup->GetIndexGroup();
                    Vector3D* vertices = triangleGroup->GetVertices();
                    const int vertexCount = triangleGroup->GetVertexCount();
                    const int triangleCount = triangleGroup->GetTriangleCount();
                    Material* material = sceneObjects[i]->GetMaterial();

#if RENDER_SIMD_CAMERA_OBJECT_PROJ
                    if (vertexCount > 0) {
                        EnsureObjectProjectionCache(static_cast<unsigned int>(vertexCount));

                        for (int v = 0; v < vertexCount; ++v) {
                            objX[v] = vertices[v].X - camPos.X;
                            objY[v] = vertices[v].Y - camPos.Y;
                            objZ[v] = vertices[v].Z - camPos.Z;
                        }

                        dsps_mulc_f32(objX, projX, vertexCount, invViewRotation.m00, 1, 1);
                        dsps_mulc_f32(objY, projY, vertexCount, invViewRotation.m01, 1, 1);
                        dsps_add_f32(projX, projY, projX, vertexCount, 1, 1, 1);
                        dsps_mulc_f32(objZ, projY, vertexCount, invViewRotation.m02, 1, 1);
                        dsps_add_f32(projX, projY, projX, vertexCount, 1, 1, 1);

                        dsps_mulc_f32(objX, projY, vertexCount, invViewRotation.m10, 1, 1);
                        dsps_mulc_f32(objY, projZ, vertexCount, invViewRotation.m11, 1, 1);
                        dsps_add_f32(projY, projZ, projY, vertexCount, 1, 1, 1);
                        dsps_mulc_f32(objZ, projZ, vertexCount, invViewRotation.m12, 1, 1);
                        dsps_add_f32(projY, projZ, projY, vertexCount, 1, 1, 1);

                        dsps_mulc_f32(objX, projZ, vertexCount, invViewRotation.m20, 1, 1);
                        dsps_mulc_f32(objY, objX, vertexCount, invViewRotation.m21, 1, 1);
                        dsps_add_f32(projZ, objX, projZ, vertexCount, 1, 1, 1);
                        dsps_mulc_f32(objZ, objX, vertexCount, invViewRotation.m22, 1, 1);
                        dsps_add_f32(projZ, objX, projZ, vertexCount, 1, 1, 1);
                    }

                    for (int j = 0; j < triangleCount; j++) {
                        const int a = indexGroup[j].A;
                        const int b = indexGroup[j].B;
                        const int c = indexGroup[j].C;

                        frameTree.Insert(Triangle2D(
                            Vector3D(projX[a], projY[a], projZ[a]),
                            Vector3D(projX[b], projY[b], projZ[b]),
                            Vector3D(projX[c], projY[c], projZ[c]),
                            &triangles[j],
                            material
                        ));
                    }
#else
                    for (int j = 0; j < triangleCount; j++) {
                        frameTree.Insert(Triangle2D(invViewRotation, camPos, &triangles[j], material));
                    }
#endif
                }
            }

            frameTree.Rebuild();
#endif

            ProtoRGBColor* colors = pixelGroup->GetColors();

            for (unsigned int i = 0; i < pixelCount; i++) {
                const Vector2D& pixelRay = cachedRays[i];
#if RENDER_CAMERA_TREE_CACHE
                Node* leafNode = cachedTree ? cachedTree->Intersect(pixelRay) : nullptr;
#else
                Node* leafNode = frameTree.Intersect(pixelRay);
#endif
                
                if (!leafNode) {
                    colors[i].R = 0;
                    colors[i].G = 0;
                    colors[i].B = 0;
                    continue;
                }

                ProtoRGBColor color = CheckRasterPixel(leafNode->GetEntities(), leafNode->GetCount(), pixelRay);

                colors[i].R = color.R;
                colors[i].G = color.G;
                colors[i].B = color.B;

            }
        }
    }
    
};
