#pragma once

#include "../Math/Rotation.h"

// Cache frame-invariant quaternion work while preserving RotateVector's exact
// arithmetic, including its original (not normalized) first coefficients.
class PreparedMaterialRotation
{
    Quaternion original;
    Quaternion unit;
    float s2 = 2.0f;
    float dPUU = 1.0f;
    bool identity = true;

public:
    void SetAngle(float angle)
    {
        original = Rotation(EulerAngles(Vector3D(0, 0, angle),
            EulerConstants::EulerOrderXYZS)).GetQuaternion();
        identity = original.IsClose(Quaternion(), Mathematics::EPSILON);
        unit = identity ? Quaternion() : original.UnitQuaternion();
        s2 = unit.W * 2.0f;
        dPUU = unit.W * unit.W - (unit.X * unit.X + unit.Y * unit.Y + unit.Z * unit.Z);
    }

    Vector3D Apply(Vector3D v) const
    {
        if (identity) return v;
        const float dPUV = (unit.X * v.X + unit.Y * v.Y + unit.Z * v.Z) * 2.0f;
        return Vector3D{
            original.X * dPUV + v.X * dPUU + ((unit.Y * v.Z) - (unit.Z * v.Y)) * s2,
            original.Y * dPUV + v.Y * dPUU + ((unit.Z * v.X) - (unit.X * v.Z)) * s2,
            original.Z * dPUV + v.Z * dPUU + ((unit.X * v.Y) - (unit.Y * v.X)) * s2
        };
    }
};
