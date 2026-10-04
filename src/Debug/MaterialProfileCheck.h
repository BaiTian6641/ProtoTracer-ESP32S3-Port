#pragma once

#ifdef PRINTINFO
#include "../Materials/SpiralMaterial.h"
#include "../Materials/GradientMaterial.h"

namespace MaterialProfileCheck
{
// The original shader is deliberately retained only in telemetry builds as an
// independent golden implementation for target/compiler equivalence and timing.
class ReferenceSpiral : public Material
{
public:
    ProtoRGBColor colors[6] = {{255,0,0}, {255,255,0}, {0,255,0}, {0,255,255}, {0,0,255}, {255,0,255}};
    float angle = 0, bend = .85f;
    ProtoRGBColor GetRGB(Vector3D position, Vector3D, Vector3D) override
    {
        if (angle != 0) {
            Quaternion temp = Rotation(EulerAngles(Vector3D(0,0,angle),
                EulerConstants::EulerOrderXYZS)).GetQuaternion();
            position = position - Vector3D(0,75,0);
            position = temp.RotateVector(position);
        }
        float radius = Vector2D(position).Magnitude();
        float theta = atan2(position.Y, position.X);
        float ratio = Mathematics::Fract(3.0f * theta / Mathematics::MPI + bend * powf(radius, .3f));
        return colors[int(floor(ratio * 6))];
    }
};

inline Vector3D Position(unsigned pixel)
{
    return Vector3D(float(pixel % 64) * 3 - 96, float(pixel / 64) * 3, 550);
}

inline bool Run()
{
    ReferenceSpiral reference;
    SpiralMaterial cached(6, reference.colors, 3, .85f);
    cached.SetPositionOffset({0,75});
    unsigned differences = 0;
    uint32_t referenceUs = 0, cachedUs = 0;
    volatile uint32_t checksum = 0;
    for (unsigned frame = 0; frame < 8; ++frame) {
        reference.angle = float(frame) * 51.43f;
        reference.bend = .8f + float(frame) * .013f;
        cached.SetRotationAngle(reference.angle);
        cached.SetBend(reference.bend);
        for (unsigned pixel = 0; pixel < 2048; ++pixel) {
            Vector3D position = Position(pixel);
            auto expected = reference.GetRGB(position, {}, {});
            auto actual = cached.GetRGB(position, {}, {});
            differences += expected.R != actual.R || expected.G != actual.G || expected.B != actual.B;
        }
        yield();
        uint32_t start = micros();
        for (unsigned pixel = 0; pixel < 2048; ++pixel) {
            auto color = reference.GetRGB(Position(pixel), {}, {});
            checksum = checksum + color.R + color.G + color.B;
        }
        referenceUs += micros() - start;
        yield();
        start = micros();
        for (unsigned pixel = 0; pixel < 2048; ++pixel) {
            auto color = cached.GetRGB(Position(pixel), {}, {});
            checksum = checksum + color.R + color.G + color.B;
        }
        cachedUs += micros() - start;
        yield();
    }
    Serial.printf("[MATERIAL_VERIFY] samples=16384 diffs=%u legacyUs=%lu cachedUs=%lu checksum=%lu\n",
        differences, static_cast<unsigned long>(referenceUs), static_cast<unsigned long>(cachedUs),
        static_cast<unsigned long>(checksum));
    return differences == 0;
}
}
#endif
