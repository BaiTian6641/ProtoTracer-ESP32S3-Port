#pragma once

#include <cmath>
#ifdef TRIANGLE3D_USE_DSP
#include <esp_dsp.h>
#endif

#include "../Math/Quaternion.h"
#include "../Math/Vector3D.h"

class Triangle3D {
private:

public:
	Vector3D* p1;
	Vector3D* p2;
	Vector3D* p3;

	Vector2D* p1UV;
	Vector2D* p2UV;
	Vector2D* p3UV;
 
	Vector3D edge1;
	Vector3D edge2;
	Vector3D normal;

	bool hasUV = false;

	Triangle3D(){}

	Triangle3D(Vector3D* p1, Vector3D* p2, Vector3D* p3) {
		this->p1 = p1;
		this->p2 = p2;
		this->p3 = p3;

		this->Normal();
	}

	Vector3D* Normal() {
		edge1 = *p2 - *p1;
		edge2 = *p3 - *p1;
		normal = edge1.CrossProduct(edge2).UnitSphere();
		
		return &normal;
	}

	bool IRAM_ATTR DidIntersect(const Vector3D& ray, const Vector3D& direction, Vector3D* intersect, Vector3D* color) {
		constexpr float kEpsilon = 0.000001f;
		const float dx = direction.X;
		const float dy = direction.Y;
		const float dz = direction.Z;

		const float e1x = edge1.X;
		const float e1y = edge1.Y;
		const float e1z = edge1.Z;
		const float e2x = edge2.X;
		const float e2y = edge2.Y;
		const float e2z = edge2.Z;

		const float pvecX = fmaf(dy, e2z, -dz * e2y);
		const float pvecY = fmaf(dz, e2x, -dx * e2z);
		const float pvecZ = fmaf(dx, e2y, -dy * e2x);

#ifdef TRIANGLE3D_USE_DSP
		// AE32 dot products for the three dot steps; padded to 4 elements for alignment.
		float det = 0.0f;
		{
			alignas(16) float a[4] = {e1x, e1y, e1z, 0.0f};
			alignas(16) float b[4] = {pvecX, pvecY, pvecZ, 0.0f};
			dsps_dotprod_f32_ae32(a, b, 3, &det);
		}
#else
		const float det = fmaf(e1x, pvecX, fmaf(e1y, pvecY, e1z * pvecZ));
#endif
		if (fabsf(det) < kEpsilon) return false;
		const float invDet = 1.0f / det;

		const float tvecX = ray.X - p1->X;
		const float tvecY = ray.Y - p1->Y;
		const float tvecZ = ray.Z - p1->Z;

#ifdef TRIANGLE3D_USE_DSP
		float u = 0.0f;
		{
			alignas(16) float a[4] = {tvecX, tvecY, tvecZ, 0.0f};
			alignas(16) float b[4] = {pvecX, pvecY, pvecZ, 0.0f};
			dsps_dotprod_f32_ae32(a, b, 3, &u);
			u *= invDet;
		}
#else
		const float u = (tvecX * pvecX + tvecY * pvecY + tvecZ * pvecZ) * invDet;
#endif
		if (u < 0.0f || u > 1.0f) return false;

		const float qvecX = fmaf(tvecY, e1z, -tvecZ * e1y);
		const float qvecY = fmaf(tvecZ, e1x, -tvecX * e1z);
		const float qvecZ = fmaf(tvecX, e1y, -tvecY * e1x);

#ifdef TRIANGLE3D_USE_DSP
		float v = 0.0f;
		{
			alignas(16) float a[4] = {dx, dy, dz, 0.0f};
			alignas(16) float b[4] = {qvecX, qvecY, qvecZ, 0.0f};
			dsps_dotprod_f32_ae32(a, b, 3, &v);
			v *= invDet;
		}
#else
		const float v = (dx * qvecX + dy * qvecY + dz * qvecZ) * invDet;
#endif
		if (v < 0.0f || u + v > 1.0f) return false;

		float t = 0.0f;
		{
#ifdef TRIANGLE3D_USE_DSP
			alignas(16) float a[4] = {e2x, e2y, e2z, 0.0f};
			alignas(16) float b[4] = {qvecX, qvecY, qvecZ, 0.0f};
			dsps_dotprod_f32_ae32(a, b, 3, &t);
			t *= invDet;
#else
			t = (e2x * qvecX + e2y * qvecY + e2z * qvecZ) * invDet;
#endif
		}
		if (t <= kEpsilon) return false;

		intersect->X = ray.X + dx * t;
		intersect->Y = ray.Y + dy * t;
		intersect->Z = ray.Z + dz * t;

		color->X = u;
		color->Y = v;
		color->Z = 1.0f - u - v;

		return true;
	}

	String ToString() {
		return p1->ToString() + " " + p2->ToString() + " " + p3->ToString();
	}
};
