#pragma once

#include <array>
#include <cmath>

namespace lime {

struct V3 {
    float x = 0, y = 0, z = 0;

    friend V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    friend V3 operator*(float s, V3 a) { return a * s; }
    friend V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
};

inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(V3 a) {
    const float l = length(a);
    return l < 1e-8f ? V3{0, 0, 0} : a * (1.0f / l);
}

struct M4 {
    std::array<std::array<float, 4>, 4> m{};

    static M4 identity() {
        M4 r;
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1;
        return r;
    }

    static M4 translation(V3 t) {
        M4 r = identity();
        r.m[3][0] = t.x;
        r.m[3][1] = t.y;
        r.m[3][2] = t.z;
        return r;
    }

    static M4 scale(V3 s) {
        M4 r = identity();
        r.m[0][0] = s.x;
        r.m[1][1] = s.y;
        r.m[2][2] = s.z;
        return r;
    }

    static M4 rotationEuler(V3 degrees) {
        const float k = 3.14159265358979f / 180.0f;
        const float cx = std::cos(degrees.x * k), sx = std::sin(degrees.x * k);
        const float cy = std::cos(degrees.y * k), sy = std::sin(degrees.y * k);
        const float cz = std::cos(degrees.z * k), sz = std::sin(degrees.z * k);

        M4 rz = identity();
        rz.m[0][0] = cz;  rz.m[0][1] = sz;
        rz.m[1][0] = -sz; rz.m[1][1] = cz;
        M4 rx = identity();
        rx.m[1][1] = cx;  rx.m[1][2] = sx;
        rx.m[2][1] = -sx; rx.m[2][2] = cx;
        M4 ry = identity();
        ry.m[0][0] = cy;  ry.m[0][2] = -sy;
        ry.m[2][0] = sy;  ry.m[2][2] = cy;
        return rz * rx * ry;
    }

    static M4 lookAt(V3 eye, V3 target, V3 up) {
        const V3 z = normalize(target - eye);
        const V3 x = normalize(cross(up, z));
        const V3 y = cross(z, x);
        M4 r = identity();
        r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x;
        r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y;
        r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z;
        r.m[3][0] = -dot(x, eye);
        r.m[3][1] = -dot(y, eye);
        r.m[3][2] = -dot(z, eye);
        return r;
    }

    static M4 perspective(float fovYDegrees, float aspect, float zn, float zf) {
        const float f = 1.0f / std::tan(fovYDegrees * 3.14159265f / 360.0f);
        M4 r;
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = zf / (zf - zn);
        r.m[2][3] = 1.0f;
        r.m[3][2] = -zn * zf / (zf - zn);
        return r;
    }

    friend M4 operator*(const M4& a, const M4& b) {
        M4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
                r.m[i][j] = s;
            }
        return r;
    }
};

inline V3 transformPoint(const M4& m, V3 p, float& wOut) {
    const float x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const float y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const float z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    wOut = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    return {x, y, z};
}

inline V3 transformDir(const M4& m, V3 d) {
    return {d.x * m.m[0][0] + d.y * m.m[1][0] + d.z * m.m[2][0],
            d.x * m.m[0][1] + d.y * m.m[1][1] + d.z * m.m[2][1],
            d.x * m.m[0][2] + d.y * m.m[1][2] + d.z * m.m[2][2]};
}

}
