#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <cstdint>
#include <cmath>
#include <type_traits>
unsigned long micros() { return 1234567; }
unsigned long millis() { return 1234; }
#include "../src/Materials/Animated/RainbowSpiral.h"
#include "../src/Materials/GradientMaterial.h"
#include "../src/Materials/Animated/SpectrumAnalyzer.h"

ProtoRGBColor palette[6] = {{255,0,0}, {255,255,0}, {0,255,0}, {0,255,255}, {0,0,255}, {255,0,255}};
bool Same(ProtoRGBColor a, ProtoRGBColor b) { return a.R == b.R && a.G == b.G && a.B == b.B; }

ProtoRGBColor LegacySpiral(Vector3D position, float rotation, float bend, float width)
{
    if (rotation != 0) {
        Quaternion temp = Rotation(EulerAngles(Vector3D(0,0,rotation), EulerConstants::EulerOrderXYZS)).GetQuaternion();
        position = position - Vector3D(0,75,0);
        position = temp.RotateVector(position);
    }
    float radius = Vector2D(position).Magnitude();
    float angle = atan2(position.Y, position.X);
    float ratio = Mathematics::Fract(width * angle / Mathematics::MPI + bend * powf(radius, .3f));
    return palette[int(floor(ratio * 6))];
}

ProtoRGBColor LegacyGradient(Vector3D position, float rotation, bool radial)
{
    if (rotation != 0) {
        Quaternion temp = Rotation(EulerAngles(Vector3D(0,0,rotation), EulerConstants::EulerOrderXYZS)).GetQuaternion();
        position = temp.RotateVector(position);
    }
    position = position - Vector3D(7,13,0);
    float pos = radial ? sqrtf(position.X * position.X + position.Y * position.Y) : position.X;
    pos = fabs(fmodf(pos, 200.0f));
    float ratio = Mathematics::Map(pos, 0.0f, 200.0f, 0.0f, 6.0f);
    uint8_t first = uint8_t(floor(ratio)), last = first + 1 >= 6 ? 0 : first + 1;
    float mu = Mathematics::Map(ratio, float(first), float(first+1), 0.0f, 1.0f);
    return ProtoRGBColor::InterpolateColors(palette[first], palette[last], mu);
}

uint32_t rng = 12345;
float Random() { rng = rng * 1664525u + 1013904223u; return float(rng & 0xffff) / 64.0f - 512.0f; }
int main()
{
    unsigned comparisons = 0;
    SpiralMaterial spiral(6, palette, 3, .85f);
    spiral.SetPositionOffset({0,75});
    GradientMaterial<6> linear(palette, 200, false), radial(palette, 200, true);
    linear.SetPositionOffset({7,13}); radial.SetPositionOffset({7,13});
    for (unsigned frame = 0; frame < 180; ++frame) {
        float angle = frame == 0 ? 0.0f : (float(frame) - 90.0f) * 4.001f;
        float bend = .8f + float(frame % 100) / 1000;
        spiral.SetRotationAngle(angle); spiral.SetBend(bend);
        linear.SetRotationAngle(angle); radial.SetRotationAngle(angle);
        PreparedMaterialRotation prepared;
        prepared.SetAngle(angle);
        for (unsigned pixel = 0; pixel < 4096; ++pixel) {
            Vector3D position(Random(), Random(), Random());
            Quaternion original = Rotation(EulerAngles(Vector3D(0,0,angle), EulerConstants::EulerOrderXYZS)).GetQuaternion();
            Vector3D expected = original.RotateVector(position), actual = prepared.Apply(position);
            if (memcmp(&expected, &actual, sizeof(Vector3D))) return 1;
            if (!Same(LegacySpiral(position, angle, bend, 3), spiral.GetRGB(position, {}, {}))) return 2;
            if (!Same(LegacyGradient(position, angle, false), linear.GetRGB(position, {}, {}))) return 3;
            if (!Same(LegacyGradient(position, angle, true), radial.GetRGB(position, {}, {}))) return 4;
            ++comparisons;
        }
    }
    // Owned JSON palette survives the source disappearing and value copies.
    auto owned = [] {
        ProtoRGBColor temporary[2] = {{200,30,90},{7,180,255}};
        return GradientMaterial<2>(temporary, 20, false, false, true);
    }();
    auto copy = owned;
    copy.HueShift(120); copy.HueShift(0); copy.UpdateRGB();
    if (!Same(copy.GetRGB({}, {}, {}), ProtoRGBColor(200,30,90))) return 5;
    GradientMaterial<2> assigned(palette, 20, false);
    assigned = owned;
    assigned.HueShift(0); assigned.UpdateRGB();
    if (!Same(assigned.GetRGB({}, {}, {}), ProtoRGBColor(200,30,90))) return 18;
    GradientMaterial<6> stepped(palette, 200, false, true);
    if (!Same(stepped.GetRGB({10,0,0}, {}, {}), palette[0])) return 19;
    ProtoRGBColor external[1] = {{10,20,30}};
    GradientMaterial<1> linked(external, 20, false);
    external[0] = {40,50,60}; linked.UpdateRGB();
    if (!Same(linked.GetRGB({}, {}, {}), external[0])) return 6;
    SpiralMaterial clone = spiral;
    Vector3D p(45,30,500);
    auto unchanged = spiral.GetRGB(p, {}, {});
    clone.HueShift(120);
    if (!Same(spiral.GetRGB(p, {}, {}), unchanged)) return 7;
    RainbowSpiral rainbow;
    rainbow.Update(.4f);
    auto before = rainbow.GetRGB(p, {}, {});
    rainbow.HueShift(120);
    if (!Same(rainbow.GetRGB(p, {}, {}), before.HueShift(120))) return 8;
    rainbow.HueShift(120);
    if (!Same(rainbow.GetRGB(p, {}, {}), before.HueShift(120))) return 9;
    rainbow.HueShift(0);
    if (!Same(rainbow.GetRGB(p, {}, {}), before)) return 10;
    bool destroyed = false;
    struct Destruction : Material {
        bool& flag;
        explicit Destruction(bool& flag) : flag(flag) {}
        ~Destruction() override { flag = true; }
        ProtoRGBColor GetRGB(Vector3D, Vector3D, Vector3D) override { return {}; }
    };
    { std::unique_ptr<Material> owner(new Destruction(destroyed)); }
    if (!destroyed) return 11;
    float bins[128] = {};
    {
        SpectrumAnalyzer spectrum({200,100}, {100,50}, true);
        if (materialAllocations) return 12;
    }
    if (materialAllocations) return 13;
    {
        SpectrumAnalyzer spectrum({200,100}, {100,50}, true);
        using Storage = typename std::aligned_storage<sizeof(BouncePhysics), alignof(BouncePhysics)>::type;
        Storage storage[128];
        for (unsigned i=0; i<128; ++i) new (&storage[i]) BouncePhysics(35,15);
        for (unsigned frame=0; frame<100; ++frame) {
            for (unsigned i=0; i<128; ++i) bins[i] = fabsf(Random()) / 512.0f;
            spectrum.Update(bins);
            if (materialAllocations != 1) return 14;
            for (unsigned i=0; i<128; ++i) {
                float expected = reinterpret_cast<BouncePhysics*>(&storage[i])->Calculate(bins[i], .1f);
                if (expected != spectrum.GetFourierData()[i]) return 15;
            }
        }
        for (unsigned i=0; i<128; ++i) reinterpret_cast<BouncePhysics*>(&storage[i])->~BouncePhysics();
    }
    if (materialAllocations) return 16;
    {
        materialAllocationFailure = true;
        SpectrumAnalyzer failed({200,100}, {100,50}, true);
        failed.Update(bins);
        if (materialAllocations || memcmp(bins, failed.GetFourierData(), sizeof(bins))) return 17;
        materialAllocationFailure = false;
    }
    printf("PASS %u positions: cached rotation bits, spiral/gradient RGB, palette lifetime/copies, hue, virtual destruction, lazy PSRAM bounce/failure\n", comparisons);
}
