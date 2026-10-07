#include <Arduino.h>
#include <cstdio>
#include <cmath>
#include <limits>
#include "../src/Math/Vector2D.h"
#include "../src/Math/Vector3D.h"

// The shim exposes only the public ESP-DSP dispatch name, with the real
// destination-before-length signature. Xtensa-specific calls cannot compile.
int main()
{
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!Mathematics::IsFinite(0) || !Mathematics::IsFinite(-12.5f) ||
        Mathematics::IsFinite(infinity) || Mathematics::IsFinite(nan) ||
        !Mathematics::IsInfinite(infinity) || !Mathematics::IsInfinite(-infinity) ||
        Mathematics::IsInfinite(nan) || !Mathematics::IsNaN(nan)) return 1;
    for (int i = -128; i <= 128; ++i) {
        const float x = i * 0.125f, y = (i + 5) * -0.25f, z = (i - 7) * 0.5f;
        Vector2D a2(x, y), b2(z, x);
        Vector3D a3(x, y, z), b3(z, x, y);
        const float expected2 = x * z + y * x;
        const float expected3 = expected2 + z * y;
        if (a2.DotProduct(b2) != expected2 || a3.DotProduct(b3) != expected3) {
            std::printf("FAIL vector dot products at %d\n", i);
            return 1;
        }
    }
    Vector2D orthogonal2(1, 0);
    Vector3D orthogonal3(0, 1, 0);
    if (orthogonal2.DotProduct(Vector2D(0, 1)) != 0 ||
        orthogonal3.DotProduct(Vector3D(1, 0, 0)) != 0) return 1;
    std::puts("PASS vector DSP dispatch: real API signature, 2/3-component results, no Xtensa symbol");
    return 0;
}
