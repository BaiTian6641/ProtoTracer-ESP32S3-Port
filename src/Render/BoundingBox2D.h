#pragma once

#include "../Math/Vector2D.h"

class BoundingBox2D{
private:
    Vector2D min;
    Vector2D max;

public:
    BoundingBox2D(){}
    BoundingBox2D(Vector2D min, Vector2D max): min(min), max(max) {}

    void UpdateBounds(Vector2D current){
        min = min.Minimum(current);
        max = max.Maximum(current);
    }
    
    Vector2D GetMinimum() const { // Added const for read-only access
        return min;
    }

    Vector2D GetMaximum() const { // Added const for read-only access
        return max;
    }

    /**
     * @brief Calculates the geometric center of the bounding box.
     * * @return Vector2D The center point of the box.
     */
    Vector2D GetCenter() const {
        return min.Add(min,max).Divide(min.Add(min,max),Vector2D(2.0f,2.0f));
    }

    bool Overlaps(const BoundingBox2D* bb) const { // Added const
        if (!bb) return false; // Safety check
        bool xOverlap = bb->GetMinimum().X < max.X && bb->GetMaximum().X > min.X;
        bool yOverlap = bb->GetMinimum().Y < max.Y && bb->GetMaximum().Y > min.Y;

        return xOverlap && yOverlap;
    }

    bool Contains(const Vector2D& v) const { // Added const
        return min.X <= v.X && v.X <= max.X && min.Y <= v.Y && v.Y <= max.Y;
    }
};