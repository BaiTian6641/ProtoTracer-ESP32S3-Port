#pragma once

#include "../Materials/Material.h"
#include "../Math/Transform.h"
#include "TriangleGroup.h"

class Object3D {
private:
    Transform transform;
    TriangleGroup* originalTriangles;
    TriangleGroup* modifiedTriangles;
    Material* material;
    bool enabled = true;

    // Double-buffer render vertices — animation writes modifiedTriangles,
    // renderer reads this stable copy. Allocated once, published each frame.
    Vector3D* mRenderVertices = nullptr;
    TriangleGroup* mRenderTriangles = nullptr;
    bool mUseDoubleBuffer = false;

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
        delete modifiedTriangles;
        if (mRenderVertices) delete[] mRenderVertices;
        if (mRenderTriangles) delete mRenderTriangles;
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
        for (int i = 0; i < modifiedTriangles->GetVertexCount(); i++) {
            modifiedTriangles->GetVertices()[i] = originalTriangles->GetVertices()[i];
        }
    }

    void UpdateTransform(){
        // Build rotation matrix from quaternion once per frame (avoids per-vertex quaternion ops)
        Quaternion rot = transform.GetRotation();
        Quaternion q = rot.UnitQuaternion();
        
        const float xx = q.X * q.X, yy = q.Y * q.Y, zz = q.Z * q.Z;
        const float xy = q.X * q.Y, xz = q.X * q.Z, yz = q.Y * q.Z;
        const float wx = q.W * q.X, wy = q.W * q.Y, wz = q.W * q.Z;
        
        const float m00 = 1.0f - 2.0f * (yy + zz);
        const float m01 = 2.0f * (xy - wz);
        const float m02 = 2.0f * (xz + wy);
        const float m10 = 2.0f * (xy + wz);
        const float m11 = 1.0f - 2.0f * (xx + zz);
        const float m12 = 2.0f * (yz - wx);
        const float m20 = 2.0f * (xz - wy);
        const float m21 = 2.0f * (yz + wx);
        const float m22 = 1.0f - 2.0f * (xx + yy);
        
        const Vector3D pos = transform.GetPosition();
        const Vector3D sc = transform.GetScale();
        const Vector3D scOff = transform.GetScaleOffset();
        const Vector3D rotOff = transform.GetRotationOffset();
        
        Vector3D* verts = modifiedTriangles->GetVertices();
        const int vCount = modifiedTriangles->GetVertexCount();
        
        for (int i = 0; i < vCount; i++) {
            // Scale: offset → scale → un-offset
            float sx = (verts[i].X - scOff.X) * sc.X + scOff.X;
            float sy = (verts[i].Y - scOff.Y) * sc.Y + scOff.Y;
            float sz = (verts[i].Z - scOff.Z) * sc.Z + scOff.Z;
            
            // Offset for rotation center
            sx -= rotOff.X; sy -= rotOff.Y; sz -= rotOff.Z;
            
            // Rotate via 3x3 matrix (replaces quaternion RotateVector)
            float rx = m00 * sx + m01 * sy + m02 * sz;
            float ry = m10 * sx + m11 * sy + m12 * sz;
            float rz = m20 * sx + m21 * sy + m22 * sz;
            
            // Un-offset rotation + translate
            verts[i].X = rx + rotOff.X + pos.X;
            verts[i].Y = ry + rotOff.Y + pos.Y;
            verts[i].Z = rz + rotOff.Z + pos.Z;
        }
    }

    TriangleGroup* GetTriangleGroup(){
        return mUseDoubleBuffer ? mRenderTriangles : modifiedTriangles;
    }

    // Allocate the render-side vertex buffer and TriangleGroup once.
    // After this, GetTriangleGroup() returns the stable render copy.
    // Call PublishVertices() after animation UpdateTransform() each frame.
    void EnableDoubleBuffer() {
        if (mUseDoubleBuffer || !modifiedTriangles) return;

        const int vc = modifiedTriangles->GetVertexCount();
        mRenderVertices = new Vector3D[vc];
        // Copy current vertices as initial state
        memcpy(mRenderVertices, modifiedTriangles->GetVertices(), vc * sizeof(Vector3D));

        mRenderTriangles = new TriangleGroup(mRenderVertices, modifiedTriangles);
        mUseDoubleBuffer = true;
    }

    // Copy animated vertices to the stable render buffer (call from animation core).
    void PublishVertices() {
        if (!mUseDoubleBuffer || !mRenderVertices || !modifiedTriangles) return;
        memcpy(mRenderVertices, modifiedTriangles->GetVertices(),
               modifiedTriangles->GetVertexCount() * sizeof(Vector3D));
    }

    Material* GetMaterial(){
        return material;
    }

    void SetMaterial(Material* material){
        this->material = material;
    }

};
