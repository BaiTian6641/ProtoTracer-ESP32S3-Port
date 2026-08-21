// host-tests/main.cpp
//
// Host-side (Windows x64, MSVC) A/B validation harness for the ProtoTracer
// render core. Compiles the REAL firmware headers (src/Math, src/Render,
// src/Materials, src/Flash/PixelGroups) against platform shims in ./shim and
// renders ONE deterministic frame of a fixed synthetic scene.
//
// The same source is built twice:
//   legacy.exe  -DDIRECT_RASTERIZER=0   (pixel-driven ray-cast + QuadTree path)
//   direct.exe  -DDIRECT_RASTERIZER=1   (triangle-driven bbox rasterizer path)
// and the 2048x3 byte color buffers are compared byte-for-byte.
//
// Scene (mirrors src/Controllers/TasESP32S3KitV1.h camera setup):
//   - PixelGroup: the real 2048-entry P3HUB75 64x32 grid (3-unit spacing),
//     so CheckDirectSupport() passes and the direct path engages.
//   - One Object3D with 4 triangles (two flat at Z=300, two tilted toward
//     Z=340) overlapping each other and the grid, each with a distinct
//     SimpleMaterial color. The overlap exercises depth arbitration and the
//     tie-breaking in both paths; the tilt exercises non-axis-aligned bboxes.
//   - Camera: Transform(pos (0,0,0), scale (1,1,1)), CameraLayout(ZForward, YUp).
//
// Output: frame file (argv[1], default "frame.bin") = 2048 * 3 bytes RGB,
// plus a small text report (FNV-1a 32 checksum, non-black pixel count,
// lit-pixel bounding box, sample pixels).

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

// Host shim first: defines IRAM_ATTR & String before any firmware header
// (mirrors the firmware, where main.cpp includes <Arduino.h> first).
#include <Arduino.h>

#include "../src/Math/Transform.h"
#include "../src/Math/Vector2D.h"   // P3HUB75.h is a raw table of Vector2D
#include "../src/Flash/PixelGroups/P3HUB75.h"
#include "../src/Render/Camera.h"
#include "../src/Render/CameraLayout.h"
#include "../src/Render/PixelGroup.h"
#include "../src/Render/Scene.h"
#include "../src/Render/Object3D.h"
#include "../src/Render/TriangleGroup.h"
#include "../src/Materials/SimpleMaterial.h"

namespace {

constexpr unsigned int kPixelCount = 2048;

// FNV-1a 32-bit
uint32_t Fnv1a32(const uint8_t* data, size_t len) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    return h;
}

// Write 2048 RGB888 pixels to file; returns true on success.
bool WriteFrame(const char* path, const ProtoRGBColor* colors) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "ERROR: cannot open %s for writing\n", path);
        return false;
    }
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(kPixelCount * 3));
    if (!buf) {
        std::fclose(f);
        return false;
    }
    for (unsigned int i = 0; i < kPixelCount; i++) {
        buf[i * 3 + 0] = static_cast<uint8_t>(colors[i].R);
        buf[i * 3 + 1] = static_cast<uint8_t>(colors[i].G);
        buf[i * 3 + 2] = static_cast<uint8_t>(colors[i].B);
    }
    const size_t written = std::fwrite(buf, 1, kPixelCount * 3, f);
    std::fclose(f);
    std::free(buf);
    return written == kPixelCount * 3;
}

} // namespace

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "frame.bin";
    std::printf("[build] DIRECT_RASTERIZER=%d\n", DIRECT_RASTERIZER);

    // ── Pixel group: the real device grid (64x32, 3-unit spacing, row-major) ──
    PixelGroup pixels(kPixelCount, P3HUB75);

    // ── Mesh: 4 triangles, all inside the grid, overlapping + tilted ──
    // Vertices (world space). Camera is at origin looking -Z after the
    // ZForward/YUp layout, so a quad at Z≈+300 with X/Y in the grid range
    // projects onto the panel.
    static const Vector3D v0(0.0f, 0.0f, 300.0f);
    static const Vector3D v1(120.0f, 0.0f, 300.0f);
    static const Vector3D v2(120.0f, 60.0f, 300.0f);
    static const Vector3D v3(0.0f, 60.0f, 300.0f);
    static const Vector3D v4(60.0f, 30.0f, 340.0f); // apex of the tilted pyramid
    static const Vector3D v5(150.0f, 60.0f, 300.0f);
    static const Vector3D v6(150.0f, 120.0f, 300.0f);
    static const Vector3D v7(90.0f, 120.0f, 300.0f);

    // NOTE on ownership: TriangleGroup's 4-arg ctor takes ownership of the
    // vertex/index arrays, and its copy ctor (used by Object3D) shares the
    // indexGroup pointer while BOTH copies' destructors delete[] it. The
    // firmware lives with this by leaking the original group; the harness
    // does the same (one-shot process, OS reclaims), so the shared
    // indexGroup is freed exactly once by the Object3D-owned copy.
    static const Vector3D vertexTable[8] = {v0, v1, v2, v3, v4, v5, v6, v7};
    static const IndexGroup indexTable[4] = {
        IndexGroup(0, 1, 2), // flat quad, first half   (red)
        IndexGroup(0, 2, 3), // flat quad, second half  (red)
        IndexGroup(1, 4, 2), // tilted triangle         (green) — overlaps red
        IndexGroup(5, 6, 7), // second flat triangle    (blue) — separate region
    };

    Vector3D* vertices = new Vector3D[8];
    for (int i = 0; i < 8; i++) vertices[i] = vertexTable[i];
    IndexGroup* indices = new IndexGroup[4];
    for (int i = 0; i < 4; i++) indices[i] = indexTable[i];

    TriangleGroup* triangles = new TriangleGroup(vertices, indices, 8, 4); // intentionally leaked

    // Four distinct materials is not possible per-triangle (one material per
    // object), so use one SimpleMaterial; the geometry overlap is what the
    // A/B test exercises. Color choice is arbitrary but non-black.
    SimpleMaterial material(ProtoRGBColor(200, 120, 60));

    Object3D object(triangles, &material);
    object.UpdateTransform(); // identity object transform; mirrors firmware pipeline

    Scene scene(2);
    scene.AddObject(&object);

    // ── Camera (mirrors TasESP32S3KitV1) ──
    Transform camT(Vector3D(0.0f, 0.0f, 0.0f), Vector3D(0.0f, 0.0f, -500.0f), Vector3D(1.0f, 1.0f, 1.0f));
    CameraLayout layout(CameraLayout::ZForward, CameraLayout::YUp);
    Camera camera(&camT, &layout, &pixels);

    // ── Render one frame ──
    camera.Rasterize(&scene);

    // ── Dump + report ──
    const ProtoRGBColor* colors = pixels.GetColors();
    if (!WriteFrame(outPath, colors)) {
        return 1;
    }

    uint8_t flat[kPixelCount * 3];
    for (unsigned int i = 0; i < kPixelCount; i++) {
        flat[i * 3 + 0] = static_cast<uint8_t>(colors[i].R);
        flat[i * 3 + 1] = static_cast<uint8_t>(colors[i].G);
        flat[i * 3 + 2] = static_cast<uint8_t>(colors[i].B);
    }

    unsigned int nonBlack = 0;
    int minX = 9999, minY = 9999, maxX = -1, maxY = -1;
    for (unsigned int i = 0; i < kPixelCount; i++) {
        if (colors[i].R || colors[i].G || colors[i].B) {
            nonBlack++;
            int x = static_cast<int>(i % 64);
            int y = static_cast<int>(i / 64);
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }

    std::printf("[frame] file=%s bytes=%u\n", outPath, static_cast<unsigned int>(kPixelCount * 3));
    std::printf("[frame] fnv1a32=0x%08x\n", Fnv1a32(flat, sizeof(flat)));
    std::printf("[frame] nonBlackPixels=%u\n", nonBlack);
    if (nonBlack > 0) {
        std::printf("[frame] litBBox=x[%d..%d] y[%d..%d]\n", minX, maxX, minY, maxY);
        const unsigned int samples[] = {0, 1, 63, 64, 128, 1024, 2047};
        for (unsigned int s : samples) {
            std::printf("[frame] pixel[%u] = (%u,%u,%u)\n", s, colors[s].R, colors[s].G, colors[s].B);
        }
    } else {
        std::fprintf(stderr, "WARNING: frame is entirely black — scene does not cover the grid\n");
    }

    return 0;
}
