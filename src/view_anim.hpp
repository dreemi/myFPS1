#pragma once
#include "raymath.h"
#include <cmath>

// Small animation helpers shared by the first-person weapon views.
namespace ViewAnim {

inline float Ease(float current, float target, float rate, float dt) {
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

// 0 before a, 1 after b, smooth in between
inline float Ramp(float t, float a, float b) {
    float x = Clamp((t - a) / (b - a), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// Like Ramp but accelerating into b (a part snapping home)
inline float RampIn(float t, float a, float b) {
    float x = Clamp((t - a) / (b - a), 0.0f, 1.0f);
    return x * x;
}

// Overshoots a little past 1 and settles (a part swinging out on its hinge)
inline float RampOutBack(float t, float a, float b) {
    float x = Clamp((t - a) / (b - a), 0.0f, 1.0f) - 1.0f;
    const float c1 = 1.1f, c3 = c1 + 1.0f;
    return 1.0f + c3 * x * x * x + c1 * x * x;
}

// A key of a handled gun's pose: seconds, camera-space offset (m), pitch, yaw, roll (deg)
struct PoseKey { float t, x, y, z, pitch, yaw, roll; };

// Channel c (0..5: x y z pitch yaw roll) at time t: a smooth curve through the
// keys (Catmull-Rom tangents, still at the ends)
inline float PoseCurve(const PoseKey* keys, int count, float t, int c) {
    auto value = [&](int i) { return (&keys[i].x)[c]; };
    if (t <= keys[0].t) return value(0);
    for (int i = 0; i < count - 1; i++) {
        const PoseKey& a = keys[i];
        const PoseKey& b = keys[i + 1];
        if (t > b.t) continue;
        float h = b.t - a.t;
        float s = (t - a.t) / h;
        auto tangent = [&](int j) {
            if (j <= 0 || j >= count - 1) return 0.0f;
            return (value(j + 1) - value(j - 1)) / (keys[j + 1].t - keys[j - 1].t);
        };
        float ma = tangent(i) * h, mb = tangent(i + 1) * h;
        float s2 = s * s, s3 = s2 * s;
        return (2 * s3 - 3 * s2 + 1) * value(i) + (s3 - 2 * s2 + s) * ma + (-2 * s3 + 3 * s2) * value(i + 1) + (s3 - s2) * mb;
    }
    return value(count - 1);
}

} // namespace ViewAnim
