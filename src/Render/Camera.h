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

#ifndef CAMERA_RASTER_WORKER
#define CAMERA_RASTER_WORKER 0
#endif

#if CAMERA_RASTER_WORKER
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

#ifndef CAMERA_RASTER_WORKER_CORE
#define CAMERA_RASTER_WORKER_CORE 0
#endif

#ifndef CAMERA_RASTER_WORKER_PRIORITY
#define CAMERA_RASTER_WORKER_PRIORITY 1
#endif

#ifndef CAMERA_RASTER_WORKER_STACK_BYTES
#define CAMERA_RASTER_WORKER_STACK_BYTES 6144
#endif

#ifndef CAMERA_RASTER_WORKER_MIN_PIXELS
#define CAMERA_RASTER_WORKER_MIN_PIXELS 512
#endif

//template<size_t pixelCount>
class Camera : public CameraBase{
private:
    Transform* transform;
    CameraLayout* cameraLayout;
    // --- MODIFIED: Pointer is to the non-templated PixelGroup class ---
    PixelGroup* pixelGroup; 
    Quaternion rayDirection;
    Quaternion lookDirection;
    Quaternion lookOffset;
    bool is2D = false;

    Vector2D* cachedRays = nullptr; // reused buffer to avoid per-frame allocations
    unsigned int cachedRayCount = 0;

    // Arena QuadTree pools — preallocated once, reset per frame (zero heap alloc in render path)
    static constexpr int kArenaMaxTriangles = 350;   // 326 triangles + margin
    static constexpr int kArenaMaxNodes = 128;        // estimated node pool
    static constexpr int kArenaMaxNodeRefs = 8192;    // entity pointer slots for all nodes
    Triangle2D mArenaTriangles[kArenaMaxTriangles];
    Node mArenaNodes[kArenaMaxNodes];
    Triangle2D** mArenaNodeRefs = nullptr;
    int mArenaNodeIdx = 0;
    int mArenaRefIdx = 0;

    // SIMD buffers for batch rotate/scale
    float* tmpX = nullptr;
    float* tmpY = nullptr;
    float* rotX = nullptr;
    float* rotY = nullptr;

#if CAMERA_RASTER_WORKER
    TaskHandle_t rasterTaskHandle = nullptr;
    SemaphoreHandle_t rasterStartSemaphore = nullptr;
    SemaphoreHandle_t rasterDoneSemaphore = nullptr;
    QuadTree* rasterWorkerTree = nullptr;
    ProtoRGBColor* rasterWorkerColors = nullptr;
    unsigned int rasterWorkerStart = 0;
    unsigned int rasterWorkerEnd = 0;
    volatile bool rasterWorkerStop = false;
#endif

    struct Rotation2D {
        float m00;
        float m01;
        float m10;
        float m11;
    };

    void EnsureRayCache() {
        const unsigned int desired = pixelGroup ? pixelGroup->GetPixelCount() : 0;
        if (desired == 0) return;

        if (cachedRayCount != desired || cachedRays == nullptr) {
            heap_caps_free(cachedRays);
            // Move to PSRAM — not used during DSP hot path, only for QuadTree intersect loop
            cachedRays = static_cast<Vector2D*>(heap_caps_malloc(desired * sizeof(Vector2D), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!cachedRays) {
                cachedRays = static_cast<Vector2D*>(heap_caps_malloc(desired * sizeof(Vector2D), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            }
            cachedRayCount = desired;
        }
    }

    void EnsureFloatCache() {
        const unsigned int desired = pixelGroup ? pixelGroup->GetPixelCount() : 0;
        if (desired == 0) return;

        if (cachedRayCount != desired || tmpX == nullptr) {
            heap_caps_free(tmpX);
            heap_caps_free(tmpY);
            heap_caps_free(rotX);
            heap_caps_free(rotY);

            tmpX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            tmpY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            rotX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
            rotY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));

            // Fallback to PSRAM if internal allocation fails
            if (!tmpX) tmpX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!tmpY) tmpY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!rotX) rotX = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!rotY) rotY = static_cast<float*>(heap_caps_malloc(desired * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
    }

    void EnsureArenaRefCache() {
        if (mArenaNodeRefs) return;

        const size_t bytes = kArenaMaxNodeRefs * sizeof(Triangle2D*);
        mArenaNodeRefs = static_cast<Triangle2D**>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!mArenaNodeRefs) {
            mArenaNodeRefs = static_cast<Triangle2D**>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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

            intersect = rayDirection.UnrotateVector(intersect);
            Vector2D uv;

            if (triangles[triangle]->hasUV){
                uv = *triangles[triangle]->p1UV * uvw.X + *triangles[triangle]->p2UV * uvw.Y + *triangles[triangle]->p3UV * uvw.Z;
            }
            
            color = triangles[triangle]->GetMaterial()->GetRGB(intersect, *triangles[triangle]->normal, Vector3D(uv.X, uv.Y, 0.0f));
        }
        
        return color;
    }

    void RasterizePixelRange(QuadTree* tree, ProtoRGBColor* colors, unsigned int begin, unsigned int end) {
        if (!tree || !colors) return;

        for (unsigned int i = begin; i < end; i++) {
            const Vector2D& pixelRay = cachedRays[i];
            Node* leafNode = tree->Intersect(pixelRay);

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

#if CAMERA_RASTER_WORKER
    static void RasterWorkerThunk(void* arg) {
        static_cast<Camera*>(arg)->RasterWorkerLoop();
    }

    void RasterWorkerLoop() {
        for (;;) {
            xSemaphoreTake(rasterStartSemaphore, portMAX_DELAY);

            if (rasterWorkerStop) {
                xSemaphoreGive(rasterDoneSemaphore);
                break;
            }

            RasterizePixelRange(rasterWorkerTree, rasterWorkerColors, rasterWorkerStart, rasterWorkerEnd);
            xSemaphoreGive(rasterDoneSemaphore);
        }

        vTaskDelete(nullptr);
    }

    bool EnsureRasterWorker() {
        if (rasterTaskHandle) return true;

        if (!rasterStartSemaphore) rasterStartSemaphore = xSemaphoreCreateBinary();
        if (!rasterDoneSemaphore) rasterDoneSemaphore = xSemaphoreCreateBinary();
        if (!rasterStartSemaphore || !rasterDoneSemaphore) return false;

        rasterWorkerStop = false;
        BaseType_t created = xTaskCreatePinnedToCore(
            RasterWorkerThunk,
            "CamRaster",
            CAMERA_RASTER_WORKER_STACK_BYTES,
            this,
            CAMERA_RASTER_WORKER_PRIORITY,
            &rasterTaskHandle,
            CAMERA_RASTER_WORKER_CORE);

        return created == pdPASS;
    }

    bool DispatchRasterWorker(QuadTree* tree, ProtoRGBColor* colors, unsigned int begin, unsigned int end) {
        if ((end - begin) < CAMERA_RASTER_WORKER_MIN_PIXELS) return false;
        if (!EnsureRasterWorker()) return false;

        rasterWorkerTree = tree;
        rasterWorkerColors = colors;
        rasterWorkerStart = begin;
        rasterWorkerEnd = end;
        xSemaphoreGive(rasterStartSemaphore);
        return true;
    }

    void WaitRasterWorker() {
        if (rasterDoneSemaphore) {
            xSemaphoreTake(rasterDoneSemaphore, portMAX_DELAY);
        }
        rasterWorkerTree = nullptr;
        rasterWorkerColors = nullptr;
    }
#endif

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
#if CAMERA_RASTER_WORKER
        if (rasterTaskHandle && rasterStartSemaphore && rasterDoneSemaphore) {
            rasterWorkerStop = true;
            xSemaphoreGive(rasterStartSemaphore);
            xSemaphoreTake(rasterDoneSemaphore, pdMS_TO_TICKS(100));
        }
        if (rasterStartSemaphore) vSemaphoreDelete(rasterStartSemaphore);
        if (rasterDoneSemaphore) vSemaphoreDelete(rasterDoneSemaphore);
#endif
        heap_caps_free(cachedRays);
        heap_caps_free(tmpX);
        heap_caps_free(tmpY);
        heap_caps_free(rotX);
        heap_caps_free(rotY);
        heap_caps_free(mArenaNodeRefs);
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
    }

    void Rasterize(Scene* scene) override {
        if (is2D){
            for (unsigned int i = 0; i < pixelGroup->GetPixelCount(); i++) {
                Vector2D pixelRay = pixelGroup->GetCoordinate(i);//scale pixel location prior to rotating and moving
                Vector3D pixelRay3D = Vector3D(pixelRay.X, pixelRay.Y, 0) + transform->GetPosition();

                ProtoRGBColor color = scene->GetObjects()[0]->GetMaterial()->GetRGB(pixelRay3D, Vector3D(), Vector3D());

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

            EnsureRayCache();
            EnsureFloatCache();
            EnsureArenaRefCache();

            const unsigned int pixelCount = pixelGroup->GetPixelCount();
            ProtoRGBColor* colors = pixelGroup->GetColors();
            if (!cachedRays || !tmpX || !tmpY || !rotX || !rotY || !colors) {
                if (colors) {
                    for (unsigned int i = 0; i < pixelCount; i++) {
                        colors[i].R = 0;
                        colors[i].G = 0;
                        colors[i].B = 0;
                    }
                }
                return;
            }

            BoundingBox2D transformedBounds;
            const Vector3D scale = transform->GetScale();
            const Rotation2D rot2 = BuildRotation2D(normLookDir);
            const Vector3D camPos = transform->GetPosition();
            const Quaternion invView = rayDirection.UnitQuaternion().Conjugate();
            // Batch scale then rotate using ESP-DSP vector ops
            for (unsigned int i = 0; i < pixelCount; ++i) {
                const Vector2D baseRay = pixelGroup->GetCoordinate(i);
                tmpX[i] = baseRay.X * scale.X;
                tmpY[i] = baseRay.Y * scale.Y;
            }

            dsps_mulc_f32(tmpX, rotX, pixelCount, rot2.m00, 1, 1);
            dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m10, 1, 1);
            dsps_add_f32(rotX, rotY, rotX, pixelCount, 1, 1, 1); // rotX = m00*x + m10*y

            dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m11, 1, 1); // reuse rotY buffer
            dsps_mulc_f32(tmpX, tmpX, pixelCount, rot2.m01, 1, 1); // tmpX now holds m01*x
            dsps_add_f32(rotY, tmpX, rotY, pixelCount, 1, 1, 1); // rotY = m11*y + m01*x

            for (unsigned int i = 0; i < pixelCount; ++i) {
                Vector2D rotatedRay(rotX[i], rotY[i]);
                cachedRays[i] = rotatedRay;
                transformedBounds.UpdateBounds(rotatedRay);
            }

            QuadTree tree(transformedBounds);
            // Use arena pools to eliminate per-frame heap allocation
            mArenaNodeIdx = 0;
            mArenaRefIdx = 0;
            if (mArenaNodeRefs) {
                tree.UseArena(mArenaTriangles, kArenaMaxTriangles,
                              mArenaNodes, &mArenaNodeIdx, kArenaMaxNodes,
                              mArenaNodeRefs, &mArenaRefIdx, kArenaMaxNodeRefs);
            }

            Object3D** objects = scene->GetCachedObjects();
            const unsigned int objectCount = scene->GetCachedObjectCount();

            //for each object in the scene, get the triangles
            for(unsigned int i = 0; i < objectCount; i++){
                Object3D* object = objects[i];
                if(object && object->IsEnabled()){
                    TriangleGroup* triangleGroup = object->GetRenderTriangleGroup();
                    Material* material = object->GetMaterial();
                    if (!triangleGroup || !material) continue;

                    Triangle3D* triangles = triangleGroup->GetTriangles();
                    const int triangleCount = triangleGroup->GetTriangleCount();
                    //for each triangle in object, project onto 2d surface, but pass material
                    for (int j = 0; j < triangleCount; j++) {
                        tree.Insert(Triangle2D(invView, camPos, &triangles[j], material));
                    }
                }
            }

            tree.Rebuild();

#if CAMERA_RASTER_WORKER
            const unsigned int workerStart = pixelCount / 2;
            const bool workerDispatched = DispatchRasterWorker(&tree, colors, workerStart, pixelCount);
            RasterizePixelRange(&tree, colors, 0, workerDispatched ? workerStart : pixelCount);
            if (workerDispatched) {
                WaitRasterWorker();
            }
#else
            RasterizePixelRange(&tree, colors, 0, pixelCount);
#endif
        }
    }
    
};
