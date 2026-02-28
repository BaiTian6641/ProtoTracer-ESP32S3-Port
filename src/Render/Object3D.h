#pragma once

#include "../Materials/Material.h"
#include "../Math/Transform.h"
#include "TriangleGroup.h"
#include <cstring>
#include <esp_dsp.h>
#include <esp_heap_caps.h>

#ifndef RENDER_SIMD_OBJECT3D
#define RENDER_SIMD_OBJECT3D 1
#endif

class Object3D {
private:
    struct Rotation3D {
        float m00;
        float m01;
        float m02;
        float m10;
        float m11;
        float m12;
        float m20;
        float m21;
        float m22;
    };

    Transform transform;
    TriangleGroup* originalTriangles;
    TriangleGroup* modifiedTriangles;
    Material* material;
    bool enabled = true;

    float* sx = nullptr;
    float* sy = nullptr;
    float* sz = nullptr;
    float* tx = nullptr;
    float* ty = nullptr;
    float* tz = nullptr;
    unsigned int scratchCount = 0;

    bool sxHeapCapsOwned = false;
    bool syHeapCapsOwned = false;
    bool szHeapCapsOwned = false;
    bool txHeapCapsOwned = false;
    bool tyHeapCapsOwned = false;
    bool tzHeapCapsOwned = false;

    void FreeBuffer(float*& buffer, bool& heapCapsOwned) {
        if (!buffer) return;

        if (heapCapsOwned) {
            heap_caps_free(buffer);
        } else {
            delete[] buffer;
        }

        buffer = nullptr;
        heapCapsOwned = false;
    }

    void AllocateBuffer(float*& buffer, bool& heapCapsOwned, unsigned int desiredCount) {
        buffer = static_cast<float*>(heap_caps_malloc(desiredCount * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        heapCapsOwned = (buffer != nullptr);

        if (!buffer) {
            buffer = new float[desiredCount];
            heapCapsOwned = false;
        }
    }

    void EnsureScratchBuffers(unsigned int desiredCount) {
        if (desiredCount == 0) return;
        if (scratchCount == desiredCount && sx && sy && sz && tx && ty && tz) return;

        FreeBuffer(sx, sxHeapCapsOwned);
        FreeBuffer(sy, syHeapCapsOwned);
        FreeBuffer(sz, szHeapCapsOwned);
        FreeBuffer(tx, txHeapCapsOwned);
        FreeBuffer(ty, tyHeapCapsOwned);
        FreeBuffer(tz, tzHeapCapsOwned);

        AllocateBuffer(sx, sxHeapCapsOwned, desiredCount);
        AllocateBuffer(sy, syHeapCapsOwned, desiredCount);
        AllocateBuffer(sz, szHeapCapsOwned, desiredCount);
        AllocateBuffer(tx, txHeapCapsOwned, desiredCount);
        AllocateBuffer(ty, tyHeapCapsOwned, desiredCount);
        AllocateBuffer(tz, tzHeapCapsOwned, desiredCount);

        if (sx && sy && sz && tx && ty && tz) {
            scratchCount = desiredCount;
        } else {
            scratchCount = 0;
        }
    }

    Rotation3D BuildRotation3D(const Quaternion& qUnit) {
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

        Rotation3D m;
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

public:
    Object3D(TriangleGroup* originalTriangles, Material* material){
        this->originalTriangles = originalTriangles;
        this->material = material;

        modifiedTriangles = new TriangleGroup(originalTriangles);
    }

    Object3D(Object3D** objects, int objectCount){
        TriangleGroup** triangleGroups = new TriangleGroup*[objectCount];

        for (int i = 0; i < objectCount; i++){
            triangleGroups[i] = objects[i]->GetTriangleGroup();
        }

        modifiedTriangles = new TriangleGroup(triangleGroups, objectCount);

        delete[] triangleGroups;
    }

    ~Object3D(){
        FreeBuffer(sx, sxHeapCapsOwned);
        FreeBuffer(sy, syHeapCapsOwned);
        FreeBuffer(sz, szHeapCapsOwned);
        FreeBuffer(tx, txHeapCapsOwned);
        FreeBuffer(ty, tyHeapCapsOwned);
        FreeBuffer(tz, tzHeapCapsOwned);
        delete modifiedTriangles;
    }

    void Enable(){
        enabled = true;
    }

    void Disable(){
        enabled = false;
    }

    bool IsEnabled(){
        return enabled;
    }

    Vector3D GetCenterOffset(){
        Vector3D center;
        
        for(int i = 0; i < modifiedTriangles->GetVertexCount(); i++) center = center + modifiedTriangles->GetVertices()[i];

        return center.Divide(modifiedTriangles->GetVertexCount());;
    }

    void GetMinMaxDimensions(Vector3D &minimum, Vector3D &maximum){
        for(int i = 0; i < modifiedTriangles->GetVertexCount(); i++){
            minimum = Vector3D::Min(minimum, modifiedTriangles->GetVertices()[i]);
            maximum = Vector3D::Max(maximum, modifiedTriangles->GetVertices()[i]);
        }
    }

    Transform* GetTransform(){
        return &transform;
    }

    void ResetVertices(){
        const int vertexCount = modifiedTriangles->GetVertexCount();
        if (vertexCount <= 0) return;

        std::memcpy(
            modifiedTriangles->GetVertices(),
            originalTriangles->GetVertices(),
            static_cast<size_t>(vertexCount) * sizeof(Vector3D)
        );
    }

    void UpdateTransform(){
        const int vertexCount = modifiedTriangles->GetVertexCount();
        if (vertexCount <= 0) return;

        Vector3D* vertices = modifiedTriangles->GetVertices();

        const Vector3D scaleOffset = transform.GetScaleOffset();
        const Vector3D scale = transform.GetScale();
        const Vector3D rotationOffset = transform.GetRotationOffset();
        Quaternion rotation = transform.GetRotation();
        const Vector3D position = transform.GetPosition();

    #if !RENDER_SIMD_OBJECT3D
        for (int i = 0; i < vertexCount; i++) {
            Vector3D modifiedVector = vertices[i];
            modifiedVector = (modifiedVector - scaleOffset) * scale + scaleOffset;
            modifiedVector = rotation.RotateVector(modifiedVector - rotationOffset) + rotationOffset;
            vertices[i] = modifiedVector + position;
        }
        return;
    #endif

        EnsureScratchBuffers(static_cast<unsigned int>(vertexCount));

        if (!(sx && sy && sz && tx && ty && tz)) {
            for (int i = 0; i < vertexCount; i++) {
                Vector3D modifiedVector = vertices[i];
                modifiedVector = (modifiedVector - scaleOffset) * scale + scaleOffset;
                modifiedVector = rotation.RotateVector(modifiedVector - rotationOffset) + rotationOffset;
                vertices[i] = modifiedVector + position;
            }
            return;
        }

        for (int i = 0; i < vertexCount; i++) {
            const Vector3D v = vertices[i];

            sx[i] = ((v.X - scaleOffset.X) * scale.X + scaleOffset.X) - rotationOffset.X;
            sy[i] = ((v.Y - scaleOffset.Y) * scale.Y + scaleOffset.Y) - rotationOffset.Y;
            sz[i] = ((v.Z - scaleOffset.Z) * scale.Z + scaleOffset.Z) - rotationOffset.Z;
        }

        const Quaternion qUnit = rotation.UnitQuaternion();
        const Rotation3D r = BuildRotation3D(qUnit);

        dsps_mulc_f32(sx, tx, vertexCount, r.m00, 1, 1);
        dsps_mulc_f32(sy, ty, vertexCount, r.m01, 1, 1);
        dsps_add_f32(tx, ty, tx, vertexCount, 1, 1, 1);
        dsps_mulc_f32(sz, ty, vertexCount, r.m02, 1, 1);
        dsps_add_f32(tx, ty, tx, vertexCount, 1, 1, 1);

        dsps_mulc_f32(sx, ty, vertexCount, r.m10, 1, 1);
        dsps_mulc_f32(sy, tz, vertexCount, r.m11, 1, 1);
        dsps_add_f32(ty, tz, ty, vertexCount, 1, 1, 1);
        dsps_mulc_f32(sz, tz, vertexCount, r.m12, 1, 1);
        dsps_add_f32(ty, tz, ty, vertexCount, 1, 1, 1);

        dsps_mulc_f32(sx, tz, vertexCount, r.m20, 1, 1);
        dsps_mulc_f32(sy, sx, vertexCount, r.m21, 1, 1);
        dsps_add_f32(tz, sx, tz, vertexCount, 1, 1, 1);
        dsps_mulc_f32(sz, sx, vertexCount, r.m22, 1, 1);
        dsps_add_f32(tz, sx, tz, vertexCount, 1, 1, 1);

        const float addX = rotationOffset.X + position.X;
        const float addY = rotationOffset.Y + position.Y;
        const float addZ = rotationOffset.Z + position.Z;

        for (int i = 0; i < vertexCount; i++) {
            vertices[i].X = tx[i] + addX;
            vertices[i].Y = ty[i] + addY;
            vertices[i].Z = tz[i] + addZ;
        }
    }

    TriangleGroup* GetTriangleGroup(){
        return modifiedTriangles;
    }

    Material* GetMaterial(){
        return material;
    }

    void SetMaterial(Material* material){
        this->material = material;
    }

};
