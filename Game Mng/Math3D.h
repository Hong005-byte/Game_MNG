#pragma once

// Minimal 3D math for the HD-2D prototype (see GameWorld3D.cpp) -- just
// enough vector/matrix machinery to build a camera view*projection matrix
// and transform world-space points through it. Deliberately hand-rolled
// instead of pulling in a dependency (GLM etc.): this project has no 3D
// math needs beyond "project a handful of building/ground vertices per
// frame", which doesn't justify a new vcpkg dependency.
//
// Everything here is plain CPU-side math -- there's no OpenGL call in this
// file. GameWorld3D.cpp uses it to compute final 2D screen-space
// coordinates + a lit color per vertex, then submits those as ordinary
// sf::VertexArray triangles through the same SFML 2D draw path every other
// pixel-art function in this codebase already uses. That's a deliberate
// choice over a real depth-buffered OpenGL pipeline -- see the "Key
// technical decision" note in the HD-2D plan: it avoids any GL state
// interop risk with the ~4000 lines of existing SFML 2D overlay code.

#include <cmath>

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        float l = length();
        return l > 1e-6f ? Vec3(x / l, y / l, z / l) : Vec3(0.f, 0.f, 0.f);
    }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// Column-major 4x4 (m[col*4 + row]) -- the layout perspective()/lookAt()
// below are written for; never handed to a GL call directly, so the
// convention only has to be consistent with itself.
struct Mat4 {
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    static Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
        Mat4 r;
        for (float& v : r.m) v = 0.f;
        float f = 1.f / std::tan(fovYRadians * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (farZ + nearZ) / (nearZ - farZ);
        r.m[11] = -1.f;
        r.m[14] = (2.f * farZ * nearZ) / (nearZ - farZ);
        return r;
    }

    static Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
        Vec3 f = (center - eye).normalized();
        Vec3 s = cross(f, up).normalized();
        Vec3 u = cross(s, f);
        Mat4 r;
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8] = s.z;   r.m[12] = -dot(s, eye);
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9] = u.z;   r.m[13] = -dot(u, eye);
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] = dot(f, eye);
        r.m[3] = 0.f;  r.m[7] = 0.f;  r.m[11] = 0.f;  r.m[15] = 1.f;
        return r;
    }

    static Mat4 multiply(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (float& v : r.m) v = 0.f;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 4; ++k)
                    r.m[col * 4 + row] += a.m[k * 4 + row] * b.m[col * 4 + k];
        return r;
    }

    // Transforms a point into clip space (x,y,z,w) -- caller does the
    // perspective divide (see projectPoint in GameWorld3D.cpp), since
    // whether/how to clip on w<=0 is a rendering concern, not a math one.
    void transformPoint(const Vec3& p, float& outX, float& outY, float& outZ, float& outW) const {
        outX = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        outY = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        outZ = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        outW = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    }
};
