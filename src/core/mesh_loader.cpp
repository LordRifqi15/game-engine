#include "core/mesh_loader.h"

#include <tiny_obj_loader.h>

#include <unistd.h>
#include <limits.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace engine {

namespace {

// Directory containing the running executable (no trailing slash).
std::string executableDir() {
    char buf[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return std::string(".");
    buf[len] = '\0';
    std::string path(buf);
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

} // namespace

Mesh loadOBJ(const std::string& path) {
    // Assets live next to the binary: <exe_dir>/../assets/... from build/, or <exe_dir>/assets/.
    const std::string exeDir = executableDir();
    std::string full = path;
    for (const std::string& base : {exeDir + "/", exeDir + "/../"}) {
        std::FILE* probe = std::fopen((base + path).c_str(), "rb");
        if (probe) {
            std::fclose(probe);
            full = base + path;
            break;
        }
    }

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, full.c_str())) {
        std::fprintf(stderr, "Fatal: failed to load OBJ '%s': %s\n", full.c_str(), err.c_str());
        std::exit(EXIT_FAILURE);
    }
    if (!warn.empty()) {
        std::fprintf(stderr, "[obj] warning loading %s: %s", full.c_str(), warn.c_str());
    }

    Mesh mesh;
    std::unordered_map<uint64_t, uint32_t> unique; // (v-index, n-index) -> vertex slot

    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            Vertex v{};
            v.position = {
                attrib.vertices[3 * index.vertex_index + 0],
                attrib.vertices[3 * index.vertex_index + 1],
                attrib.vertices[3 * index.vertex_index + 2],
            };

            if (index.normal_index >= 0) {
                v.normal = {
                    attrib.normals[3 * index.normal_index + 0],
                    attrib.normals[3 * index.normal_index + 1],
                    attrib.normals[3 * index.normal_index + 2],
                };
            }

            // UVs: default (0,0) when absent. OBJ v is top-down; flip for GL-style.
            if (index.texcoord_index >= 0) {
                v.uv = {
                    attrib.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * index.texcoord_index + 1],
                };
            }

            const uint64_t key =
                (static_cast<uint64_t>(static_cast<uint32_t>(index.vertex_index)) << 40) |
                (static_cast<uint64_t>(static_cast<uint32_t>(index.normal_index + 1)) << 20) |
                static_cast<uint64_t>(static_cast<uint32_t>(index.texcoord_index + 1));
            auto [it, inserted] = unique.emplace(key, static_cast<uint32_t>(mesh.vertices.size()));
            if (inserted) {
                mesh.vertices.push_back(v);
            }
            mesh.indices.push_back(it->second);
        }
    }

    // Generate flat face normals when the file had none.
    bool hasNormals = false;
    for (const auto& v : mesh.vertices) {
        if (v.normal != glm::vec3{0.0f}) {
            hasNormals = true;
            break;
        }
    }
    if (!hasNormals) {
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            Vertex& a = mesh.vertices[mesh.indices[i]];
            Vertex& b = mesh.vertices[mesh.indices[i + 1]];
            Vertex& c = mesh.vertices[mesh.indices[i + 2]];
            glm::vec3 n = glm::normalize(glm::cross(b.position - a.position, c.position - a.position));
            a.normal = b.normal = c.normal = n;
        }
    }

    std::printf("[obj] loaded %s: %zu verts, %zu indices\n",
                full.c_str(), mesh.vertices.size(), mesh.indices.size());
    return mesh;
}

} // namespace engine
