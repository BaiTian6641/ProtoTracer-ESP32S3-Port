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
#include <ProtoGC.h>

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

// ── Direct triangle rasterizer (Jet-derived) ──
// When enabled (default) and the pixel group is the regular 64x32 HUB75
// grid, the camera renders triangle-driven (project → pixel bbox →
// barycentric test with a per-pixel depth buffer) instead of the legacy
// pixel-driven ray-cast + QuadTree path. Work scales with covered pixels
// instead of pixelCount × leaf-triangle tests, and there is no tree build.
// Define DIRECT_RASTERIZER=0 to force the legacy QuadTree path (A/B).
#ifndef DIRECT_RASTERIZER
#define DIRECT_RASTERIZER 1
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

#if DIRECT_RASTERIZER
    // Direct-rasterizer state (triangle-driven, no QuadTree).
    // Depth buffer stores the closest triangle's average depth per pixel
    // (smaller = closer), matching the legacy CheckRasterPixel semantics.
    float* mZBuffer = nullptr;
    bool mDirectSupported = false;
    bool mDirectChecked = false;
    float mGridSpacing = 1.0f;
    static constexpr unsigned int kGridCols = 64;
    static constexpr unsigned int kGridRows = 32;

    // Verify the pixel group is the regular row-major HUB75 grid expected
    // by the direct rasterizer (spacing derived from the first two pixels;
    // P3HUB75 is a strict 3-unit grid). One-time check at first frame.
    bool CheckDirectSupport() {
        if (!pixelGroup) return false;
        const unsigned int pixelCount = pixelGroup->GetPixelCount();
        if (pixelCount != kGridCols * kGridRows) return false;
        const float s = pixelGroup->GetCoordinate(1).X - pixelGroup->GetCoordinate(0).X;
        if (s <= 0.0f) return false;
        for (unsigned int i = 0; i < pixelCount; i++) {
            const Vector2D p = pixelGroup->GetCoordinate(i);
            if (p.X != (float)(i % kGridCols) * s || p.Y != (float)(i / kGridCols) * s) return false;
        }
        mGridSpacing = s;
        return true;
    }

    void EnsureZBuffer() {
        if (mZBuffer) return;
        mZBuffer = static_cast<float*>(protogc::ProtoGC::internalAlloc(kGridCols * kGridRows * sizeof(float)));
        if (!mZBuffer) {
            mZBuffer = static_cast<float*>(protogc::ProtoGC::psramAlloc(kGridCols * kGridRows * sizeof(float)));
        }
    }
#endif

    void EnsureRayCache() {
        const unsigned int desired = pixelGroup ? pixelGroup->GetPixelCount() : 0;
        if (desired == 0) return;

        if (cachedRayCount != desired || cachedRays == nullptr) {
            protogc::ProtoGC::heapFree(cachedRays);
            // Move to PSRAM — not used during DSP hot path, only for QuadTree intersect loop
            cachedRays = static_cast<Vector2D*>(protogc::ProtoGC::psramAlloc(desired * sizeof(Vector2D)));
            if (!cachedRays) {
                cachedRays = static_cast<Vector2D*>(protogc::ProtoGC::internalAlloc(desired * sizeof(Vector2D)));
            }
            cachedRayCount = desired;
        }
    }

    void EnsureFloatCache() {
        const unsigned int desired = pixelGroup ? pixelGroup->GetPixelCount() : 0;
        if (desired == 0) return;

        if (cachedRayCount != desired || tmpX == nullptr) {
            protogc::ProtoGC::heapFree(tmpX);
            protogc::ProtoGC::heapFree(tmpY);
            protogc::ProtoGC::heapFree(rotX);
            protogc::ProtoGC::heapFree(rotY);

            tmpX = static_cast<float*>(protogc::ProtoGC::internalAlloc(desired * sizeof(float)));
            tmpY = static_cast<float*>(protogc::ProtoGC::internalAlloc(desired * sizeof(float)));
            rotX = static_cast<float*>(protogc::ProtoGC::internalAlloc(desired * sizeof(float)));
            rotY = static_cast<float*>(protogc::ProtoGC::internalAlloc(desired * sizeof(float)));

            // Fallback to PSRAM if internal allocation fails
            if (!tmpX) tmpX = static_cast<float*>(protogc::ProtoGC::psramAlloc(desired * sizeof(float)));
            if (!tmpY) tmpY = static_cast<float*>(protogc::ProtoGC::psramAlloc(desired * sizeof(float)));
            if (!rotX) rotX = static_cast<float*>(protogc::ProtoGC::psramAlloc(desired * sizeof(float)));
            if (!rotY) rotY = static_cast<float*>(protogc::ProtoGC::psramAlloc(desired * sizeof(float)));
        }
    }

    void EnsureArenaRefCache() {
        if (mArenaNodeRefs) return;

        const size_t bytes = kArenaMaxNodeRefs * sizeof(Triangle2D*);
        mArenaNodeRefs = static_cast<Triangle2D**>(protogc::ProtoGC::psramAlloc(bytes));
        if (!mArenaNodeRefs) {
            mArenaNodeRefs = static_cast<Triangle2D**>(protogc::ProtoGC::internalAlloc(bytes));
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

#if DIRECT_RASTERIZER
    // Triangle-driven rasterization for the regular HUB75 grid.
    // Caller must set lookDirection/rayDirection first (see Rasterize()).
    void RasterizeDirect(Scene* scene) {
        EnsureRayCache();
        EnsureFloatCache();
        EnsureZBuffer();

        const unsigned int pixelCount = pixelGroup->GetPixelCount();
        ProtoRGBColor* colors = pixelGroup->GetColors();
        if (!cachedRays || !tmpX || !tmpY || !rotX || !rotY || !colors || !mZBuffer) {
            // Allocator failure: fall back to a safe black frame.
            if (colors) {
                for (unsigned int i = 0; i < pixelCount; i++) {
                    colors[i].R = 0; colors[i].G = 0; colors[i].B = 0;
                }
            }
            return;
        }

        // ── Ray preparation (same math as the legacy path) ──
        const Vector3D scale = transform->GetScale();
        const Rotation2D rot2 = BuildRotation2D(lookDirection.UnitQuaternion());
        for (unsigned int i = 0; i < pixelCount; ++i) {
            const Vector2D baseRay = pixelGroup->GetCoordinate(i);
            tmpX[i] = baseRay.X * scale.X;
            tmpY[i] = baseRay.Y * scale.Y;
        }
        dsps_mulc_f32(tmpX, rotX, pixelCount, rot2.m00, 1, 1);
        dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m10, 1, 1);
        dsps_add_f32(rotX, rotY, rotX, pixelCount, 1, 1, 1); // rotX = m00*x + m10*y
        dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m11, 1, 1);
        dsps_mulc_f32(tmpX, tmpX, pixelCount, rot2.m01, 1, 1);
        dsps_add_f32(rotY, tmpX, rotY, pixelCount, 1, 1, 1); // rotY = m11*y + m01*x

        // Full-frame clear: triangle-driven writes are sparse, and the
        // depth buffer starts empty (legacy writes every pixel each frame).
        for (unsigned int i = 0; i < pixelCount; ++i) {
            cachedRays[i] = Vector2D(rotX[i], rotY[i]);
            colors[i].R = 0; colors[i].G = 0; colors[i].B = 0;
            mZBuffer[i] = Mathematics::FLTMAX;
        }

        const Quaternion invView = rayDirection.UnitQuaternion().Conjugate();
        const Vector3D camPos = transform->GetPosition();
        const float gridScale = 1.0f / mGridSpacing;
        const int maxCol = (int)kGridCols - 1;
        const int maxRow = (int)kGridRows - 1;

        Object3D** objects = scene->GetCachedObjects();
        const unsigned int objectCount = scene->GetCachedObjectCount();

        for (unsigned int i = 0; i < objectCount; i++) {
            Object3D* object = objects[i];
            if (!object || !object->IsEnabled()) continue;
            TriangleGroup* triangleGroup = object->GetRenderTriangleGroup();
            Material* material = object->GetMaterial();
            if (!triangleGroup || !material) continue;

            Triangle3D* triangles = triangleGroup->GetTriangles();
            const int triangleCount = triangleGroup->GetTriangleCount();

            for (int j = 0; j < triangleCount; j++) {
                Triangle2D t2(invView, camPos, &triangles[j], material);

                // Pixel bbox: only pixels whose centers can fall inside.
                const Vector2D p1 = t2.GetP1();
                const Vector2D p2 = t2.GetP2();
                const Vector2D p3 = t2.GetP3();
                const float minX = Mathematics::Min(p1.X, p2.X, p3.X);
                const float maxX = Mathematics::Max(p1.X, p2.X, p3.X);
                const float minY = Mathematics::Min(p1.Y, p2.Y, p3.Y);
                const float maxY = Mathematics::Max(p1.Y, p2.Y, p3.Y);

                int pxMin = (int)ceilf(minX * gridScale);
                int pxMax = (int)floorf(maxX * gridScale);
                int pyMin = (int)ceilf(minY * gridScale);
                int pyMax = (int)floorf(maxY * gridScale);
                if (pxMin < 0) pxMin = 0;
                if (pyMin < 0) pyMin = 0;
                if (pxMax > maxCol) pxMax = maxCol;
                if (pyMax > maxRow) pyMax = maxRow;
                if (pxMin > pxMax || pyMin > pyMax) continue;

                const float avgZ = t2.averageDepth;

                for (int py = pyMin; py <= pyMax; py++) {
                    const unsigned int rowBase = (unsigned int)py * kGridCols;
                    for (int px = pxMin; px <= pxMax; px++) {
                        const unsigned int idx = rowBase + (unsigned int)px;
                        // Depth arbitration identical to legacy CheckRasterPixel:
                        // strictly closer average depth wins; equal depth keeps
                        // the first triangle (later ones fail the strict test).
                        if (mZBuffer[idx] <= avgZ) continue;
                        const Vector2D& ray = cachedRays[idx];

                        float u = 0.0f, v = 0.0f, w = 0.0f;
                        if (!t2.DidIntersect(ray.X, ray.Y, u, v, w)) continue;

                        mZBuffer[idx] = avgZ;

                        Vector3D intersect = (*t2.t3p1 * u) + (*t2.t3p2 * v) + (*t2.t3p3 * w);
                        intersect = rayDirection.UnrotateVector(intersect);

                        Vector2D uv;
                        if (t2.hasUV) uv = (*t2.p1UV * u) + (*t2.p2UV * v) + (*t2.p3UV * w);

                        const ProtoRGBColor color = t2.material->GetRGB(intersect, *t2.normal, Vector3D(uv.X, uv.Y, 0.0f));
                        colors[idx].R = color.R;
                        colors[idx].G = color.G;
                        colors[idx].B = color.B;
                    }
                }
            }
        }
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
        protogc::ProtoGC::heapFree(cachedRays);
        protogc::ProtoGC::heapFree(tmpX);
        protogc::ProtoGC::heapFree(tmpY);
        protogc::ProtoGC::heapFree(rotX);
        protogc::ProtoGC::heapFree(rotY);
        protogc::ProtoGC::heapFree(mArenaNodeRefs);
#if DIRECT_RASTERIZER
        protogc::ProtoGC::heapFree(mZBuffer);
#endif
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

#if DIRECT_RASTERIZER
            // Triangle-driven path for the regular HUB75 grid; the legacy
            // pixel-driven QuadTree path below remains as fallback and for
            // A/B comparison (build with -DDIRECT_RASTERIZER=0).
            if (!mDirectChecked) {
                mDirectSupported = CheckDirectSupport();
                mDirectChecked = true;
#if defined(ARDUINO)
                Serial.printf("[RASTER] direct=%d gridSpacing=%.2f\n", mDirectSupported ? 1 : 0, mGridSpacing);
#endif
            }
            if (mDirectSupported) {
                RasterizeDirect(scene);
                return;
            }
#endif

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
