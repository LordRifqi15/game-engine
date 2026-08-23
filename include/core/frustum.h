#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>

#include <array>

namespace engine {

// View frustum: 6 planes (left, right, bottom, top, near, far) extracted from
// a view-projection matrix. Plane = vec4(a,b,c,d) with ax+by+cz+d = 0 and
// (a,b,c) normalized; negative side = outside.
struct Frustum {
    std::array<glm::vec4, 6> planes;

    static Frustum fromViewProjection(const glm::mat4& vp) {
        Frustum f;
        // Gribb-Hartmann extraction: rows of the matrix combine into planes.
        f.planes[0] = row(vp, 3) + row(vp, 0); // left
        f.planes[1] = row(vp, 3) - row(vp, 0); // right
        f.planes[2] = row(vp, 3) + row(vp, 1); // bottom
        f.planes[3] = row(vp, 3) - row(vp, 1); // top
        f.planes[4] = row(vp, 3) + row(vp, 2); // near
        f.planes[5] = row(vp, 3) - row(vp, 2); // far
        for (auto& p : f.planes) {
            float len = glm::length(glm::vec3(p));
            if (len > 0.0f) p /= len;
        }
        return f;
    }

    // Sphere vs. all planes: outside if signed distance < -radius for any plane.
    bool intersectsSphere(const glm::vec3& center, float radius) const {
        for (const auto& p : planes) {
            float dist = p.x * center.x + p.y * center.y + p.z * center.z + p.w;
            if (dist < -radius) return false;
        }
        return true;
    }

private:
    static glm::vec4 row(const glm::mat4& m, int i) {
        // glm is column-major; "row i" of the matrix as written = column access [i].
        return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]);
    }
};

} // namespace engine
