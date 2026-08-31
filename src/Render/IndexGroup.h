#pragma once

#include <cstdint>

// Triangle vertex indices. Stored as uint16 (vertex counts are < 65536; the
// face has 495). This halves the index buffer (6276 B -> 3138 B for 523 tris)
// and is read only at mesh-construction time (the render hot path uses the
// already-wired vertex pointers), so narrowing is render-path neutral.
typedef struct IndexGroup {
public:
	uint16_t A = 0;
	uint16_t B = 0;
	uint16_t C = 0;

    IndexGroup() {
        this->A = 0;
        this->B = 0;
        this->C = 0;
    }

    IndexGroup(const IndexGroup& indexGroup) {
        this->A = indexGroup.A;
        this->B = indexGroup.B;
        this->C = indexGroup.C;
    }

    IndexGroup(unsigned int X, unsigned int Y, unsigned int Z) {
        this->A = (uint16_t)X;
        this->B = (uint16_t)Y;
        this->C = (uint16_t)Z;
    }

    IndexGroup Add(IndexGroup indexGroup) {
        return IndexGroup {
            (uint16_t)(this->A + indexGroup.A),
            (uint16_t)(this->B + indexGroup.B),
            (uint16_t)(this->C + indexGroup.C)
        };
    }

    IndexGroup Subtract(IndexGroup indexGroup) {
        return IndexGroup {
            (uint16_t)(this->A - indexGroup.A),
            (uint16_t)(this->B - indexGroup.B),
            (uint16_t)(this->C - indexGroup.C)
        };
    }

    IndexGroup Multiply(IndexGroup indexGroup) {
        return IndexGroup {
            (uint16_t)(this->A * indexGroup.A),
            (uint16_t)(this->B * indexGroup.B),
            (uint16_t)(this->C * indexGroup.C)
        };
    }

    IndexGroup Divide(IndexGroup indexGroup) {
        return IndexGroup {
            (uint16_t)(this->A / indexGroup.A),
            (uint16_t)(this->B / indexGroup.B),
            (uint16_t)(this->C / indexGroup.C)
        };
    }

    String ToString() {
        return "[" + String(A) + ", " + String(B) + ", " + String(C) + "]";
    }

} IndexGroup;
