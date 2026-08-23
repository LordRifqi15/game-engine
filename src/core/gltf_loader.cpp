#include "core/gltf_loader.h"

#include <unistd.h>
#include <limits.h>
#include <cmath>

#include "renderer/vulkan/texture_cache.h"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace engine {

namespace {

std::string resolveAssetPath(const std::string& path) {
    char buf[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exeDir = len > 0 ? std::string(buf, static_cast<size_t>(len)) : ".";
    size_t slash = exeDir.find_last_of('/');
    exeDir = slash == std::string::npos ? "." : exeDir.substr(0, slash);

    for (const std::string& base : {exeDir + "/", exeDir + "/../"}) {
        std::FILE* probe = std::fopen((base + path).c_str(), "rb");
        if (probe) {
            std::fclose(probe);
            return base + path;
        }
    }
    return path;
}

} // namespace

std::vector<GLTFPrimitive> loadGLTF(const std::string& path, TextureCache* textures) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    std::string full = resolveAssetPath(path);

    bool ret = loader.LoadASCIIFromFile(&model, &err, &warn, full);
    if (!ret && full.size() > 4 && full.substr(full.size() - 4) == ".glb") {
        // try binary
        ret = loader.LoadBinaryFromFile(&model, &err, &warn, full);
    }
    if (!warn.empty()) std::fprintf(stderr, "[gltf] warning: %s\n", warn.c_str());
    if (!ret) {
        std::fprintf(stderr, "Fatal: failed to load GLTF '%s': %s\n", full.c_str(), err.c_str());
        std::exit(EXIT_FAILURE);
    }

    std::vector<GLTFPrimitive> out;

    for (const auto& gltfMesh : model.meshes) {
        for (const auto& primitive : gltfMesh.primitives) {
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;

            GLTFPrimitive outPrim;

            // --- positions / normals / uvs from attributes ---
            const auto& posIt = primitive.attributes.find("POSITION");
            const auto& normIt = primitive.attributes.find("NORMAL");
            const auto& uvIt = primitive.attributes.find("TEXCOORD_0");
            if (posIt == primitive.attributes.end()) continue;

            const auto& posAcc = model.accessors[posIt->second];
            size_t vertexCount = posAcc.count;
            outPrim.mesh.vertices.resize(vertexCount);

            {
                const auto& acc = posAcc;
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data =
                    reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset]);
                for (size_t v = 0; v < vertexCount; ++v) {
                    size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv) / sizeof(float) : 3;
                    size_t off = v * stride;
                    outPrim.mesh.vertices[v].position = {data[off], data[off + 1], data[off + 2]};
                }
            }

            if (normIt != primitive.attributes.end()) {
                const auto& acc = model.accessors[normIt->second];
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data =
                    reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset]);
                for (size_t v = 0; v < vertexCount; ++v) {
                    size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv) / sizeof(float) : 3;
                    outPrim.mesh.vertices[v].normal = {data[v * stride], data[v * stride + 1],
                                                       data[v * stride + 2]};
                }
            } else {
                // flat normals per triangle
                for (size_t i = 0; i + 2 < outPrim.mesh.indices.size(); i += 3) {}
            }

            if (uvIt != primitive.attributes.end()) {
                const auto& acc = model.accessors[uvIt->second];
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data =
                    reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset]);
                for (size_t v = 0; v < vertexCount; ++v) {
                    size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv) / sizeof(float) : 2;
                    outPrim.mesh.vertices[v].uv = {data[v * stride], data[v * stride + 1]};
                }
            }

            // --- indices ---
            const auto& idxAcc = model.accessors[primitive.indices];
            const auto& idxBv = model.bufferViews[idxAcc.bufferView];
            const unsigned char* idxData =
                &model.buffers[idxBv.buffer].data[idxBv.byteOffset + idxAcc.byteOffset];

            switch (idxAcc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                    const uint32_t* d = reinterpret_cast<const uint32_t*>(idxData);
                    outPrim.mesh.indices.assign(d, d + idxAcc.count);
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    const uint16_t* d = reinterpret_cast<const uint16_t*>(idxData);
                    for (size_t i = 0; i < idxAcc.count; ++i)
                        outPrim.mesh.indices.push_back(d[i]);
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                    for (size_t i = 0; i < idxAcc.count; ++i)
                        outPrim.mesh.indices.push_back(idxData[i]);
                    break;
                }
                default:
                    std::fprintf(stderr, "[gltf] unsupported index type in %s\n", full.c_str());
                    continue;
            }

            // Generate face normals when missing.
            bool hasNormals = false;
            for (const auto& v : outPrim.mesh.vertices)
                if (v.normal != glm::vec3{0.0f}) { hasNormals = true; break; }
            if (!hasNormals) {
                for (size_t i = 0; i + 2 < outPrim.mesh.indices.size(); i += 3) {
                    Vertex& a = outPrim.mesh.vertices[outPrim.mesh.indices[i]];
                    Vertex& b = outPrim.mesh.vertices[outPrim.mesh.indices[i + 1]];
                    Vertex& c = outPrim.mesh.vertices[outPrim.mesh.indices[i + 2]];
                    glm::vec3 n = glm::normalize(
                        glm::cross(b.position - a.position, c.position - a.position));
                    a.normal = b.normal = c.normal = n;
                }
            }

            // --- material ---
            if (primitive.material >= 0 &&
                primitive.material < static_cast<int>(model.materials.size())) {
                const auto& mat = model.materials[primitive.material];
                const auto& pbr = mat.pbrMetallicRoughness;

                if (pbr.baseColorFactor.size() >= 3) {
                    // GLTF colors are linear; engine works in sRGB-ish space — pass through.
                    outPrim.material.baseColor = {static_cast<float>(pbr.baseColorFactor[0]),
                                                  static_cast<float>(pbr.baseColorFactor[1]),
                                                  static_cast<float>(pbr.baseColorFactor[2]),
                                                  pbr.baseColorFactor.size() > 3
                                                      ? static_cast<float>(pbr.baseColorFactor[3])
                                                      : 1.0f};
                }
                outPrim.material.metallic = static_cast<float>(pbr.metallicFactor);
                outPrim.material.roughness = static_cast<float>(pbr.roughnessFactor);

                if (textures && pbr.baseColorTexture.index >= 0 &&
                    pbr.baseColorTexture.index < static_cast<int>(model.textures.size())) {
                    const auto& tex = model.textures[pbr.baseColorTexture.index];
                    if (tex.source >= 0 && tex.source < static_cast<int>(model.images.size())) {
                        const auto& image = model.images[tex.source];
                        // Embedded buffers get a synthetic key; file refs use their URI.
                        std::string texKey = image.uri.empty()
                                                 ? full + "#img" + std::to_string(tex.source)
                                                 : image.uri;
                        std::string dir = full.substr(0, full.find_last_of('/') + 1);
                        std::string texPath =
                            image.uri.empty() ? texKey : dir + image.uri;

                        int tw, th, tc;
                        stbi_uc* pixels = stbi_load(texPath.c_str(), &tw, &th, &tc, STBI_rgb_alpha);
                        if (pixels) {
                            outPrim.material.baseColorTexture =
                                textures->createFromPixels(texKey, pixels, tw, th);
                            stbi_image_free(pixels);
                        } else {
                            std::fprintf(stderr, "[gltf] failed to load texture %s\n",
                                         texPath.c_str());
                        }
                    }
                }
            }

            std::printf("[gltf] %s primitive: %zu verts, %zu indices\n",
                        full.c_str(), outPrim.mesh.vertices.size(),
                        outPrim.mesh.indices.size());
            out.push_back(std::move(outPrim));
        }
    }

    if (out.empty()) {
        std::fprintf(stderr, "[gltf] no triangle primitives found in %s\n", full.c_str());
    }
    return out;
}

} // namespace engine
