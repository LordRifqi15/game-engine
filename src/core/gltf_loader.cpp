#include "core/gltf_loader.h"

#include <unistd.h>
#include <limits.h>
#include <cmath>

#include "renderer/vulkan/texture_cache.h"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <string>
#include <unordered_map>

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

// Helpers to read accessor data
const unsigned char* accessorBytes(const tinygltf::Model& m, int accIdx, size_t& byteStride, size_t& count, int& componentType, int& type) {
    const auto& acc = m.accessors[accIdx];
    const auto& bv = m.bufferViews[acc.bufferView];
    const unsigned char* base = m.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset;
    byteStride = acc.ByteStride(bv);
    count = acc.count;
    componentType = acc.componentType;
    type = acc.type;
    return base;
}

glm::mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    if (!node.matrix.empty()) {
        glm::mat4 m(1.0f);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = static_cast<float>(node.matrix[c * 4 + r]);
        return m;
    }
    glm::vec3 t{0.0f}, s{1.0f};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    if (node.translation.size() == 3) t = {float(node.translation[0]), float(node.translation[1]), float(node.translation[2])};
    if (node.scale.size() == 3) s = {float(node.scale[0]), float(node.scale[1]), float(node.scale[2])};
    if (node.rotation.size() == 4) r = glm::quat(float(node.rotation[3]), float(node.rotation[0]), float(node.rotation[1]), float(node.rotation[2]));
    glm::mat4 tm = glm::translate(glm::mat4(1.0f), t);
    glm::mat4 rm = glm::mat4_cast(r);
    glm::mat4 sm = glm::scale(glm::mat4(1.0f), s);
    return tm * rm * sm;
}

} // namespace

GLTFModel loadGLTFModel(const std::string& path, TextureCache* textures) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    std::string full = resolveAssetPath(path);
    bool ret = loader.LoadASCIIFromFile(&model, &err, &warn, full);
    if (!ret && full.size() > 4 && full.substr(full.size() - 4) == ".glb") {
        ret = loader.LoadBinaryFromFile(&model, &err, &warn, full);
    }
    if (!warn.empty()) std::fprintf(stderr, "[gltf] warning: %s\n", warn.c_str());
    GLTFModel outModel;
    if (!ret) {
        outModel.error = err;
        std::fprintf(stderr, "[gltf] failed to load %s: %s\n", full.c_str(), err.c_str());
        return outModel;
    }

    // --- primitives (with skinning attributes) ---
    for (const auto& gltfMesh : model.meshes) {
        for (const auto& primitive : gltfMesh.primitives) {
            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) continue;
            GLTFPrimitive outPrim;
            const auto& posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end()) continue;
            const auto& posAcc = model.accessors[posIt->second];
            size_t vertexCount = posAcc.count;
            outPrim.mesh.vertices.resize(vertexCount);
            // default joints/weights to zero (shader will handle)
            for (auto& v : outPrim.mesh.vertices) {
                v.jointIndices = glm::vec4(0.0f);
                v.jointWeights = glm::vec4(0.0f);
            }
            {
                const auto& acc = posAcc;
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data = reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset + acc.byteOffset]);
                for (size_t v = 0; v < vertexCount; ++v) {
                    size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv) / sizeof(float) : 3;
                    size_t off = v * stride;
                    outPrim.mesh.vertices[v].position = {data[off], data[off + 1], data[off + 2]};
                }
            }
            auto normIt = primitive.attributes.find("NORMAL");
            if (normIt != primitive.attributes.end()) {
                const auto& acc = model.accessors[normIt->second];
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data = reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset + acc.byteOffset]);
                for (size_t v = 0; v < vertexCount; ++v) {
                    size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv) / sizeof(float) : 3;
                    outPrim.mesh.vertices[v].normal = {data[v * stride], data[v * stride + 1], data[v * stride + 2]};
                }
            }
            auto uvIt = primitive.attributes.find("TEXCOORD_0");
            if (uvIt != primitive.attributes.end()) {
                const auto& acc = model.accessors[uvIt->second];
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data = reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset + acc.byteOffset]);
                for (size_t v = 0; v < vertexCount; ++v) {
                    size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv) / sizeof(float) : 2;
                    outPrim.mesh.vertices[v].uv = {data[v * stride], data[v * stride + 1]};
                }
            }
            // JOINTS_0 and WEIGHTS_0
            auto jointsIt = primitive.attributes.find("JOINTS_0");
            auto weightsIt = primitive.attributes.find("WEIGHTS_0");
            if (jointsIt != primitive.attributes.end() && weightsIt != primitive.attributes.end()) {
                const auto& jAcc = model.accessors[jointsIt->second];
                const auto& jBv = model.bufferViews[jAcc.bufferView];
                const unsigned char* jBase = &model.buffers[jBv.buffer].data[jBv.byteOffset + jAcc.byteOffset];
                size_t jStride = jAcc.ByteStride(jBv) ? jAcc.ByteStride(jBv) : 0;
                // Determine tightly packed size
                size_t jElemSize = 0;
                if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) jElemSize = 4;
                else if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) jElemSize = 8;
                else if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) jElemSize = 16;
                if (jStride == 0) jStride = jElemSize;

                const auto& wAcc = model.accessors[weightsIt->second];
                const auto& wBv = model.bufferViews[wAcc.bufferView];
                const float* wBase = reinterpret_cast<const float*>(&model.buffers[wBv.buffer].data[wBv.byteOffset + wAcc.byteOffset]);
                size_t wStride = wAcc.ByteStride(wBv) ? wAcc.ByteStride(wBv) / sizeof(float) : 4;

                for (size_t v = 0; v < vertexCount; ++v) {
                    glm::vec4 indices{0.0f};
                    if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(jBase + v * jStride);
                        indices = glm::vec4(float(ptr[0]), float(ptr[1]), float(ptr[2]), float(ptr[3]));
                    } else if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                        const uint16_t* ptr = reinterpret_cast<const uint16_t*>(jBase + v * jStride);
                        indices = glm::vec4(float(ptr[0]), float(ptr[1]), float(ptr[2]), float(ptr[3]));
                    } else if (jAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                        const uint32_t* ptr = reinterpret_cast<const uint32_t*>(jBase + v * jStride);
                        indices = glm::vec4(float(ptr[0]), float(ptr[1]), float(ptr[2]), float(ptr[3]));
                    }
                    outPrim.mesh.vertices[v].jointIndices = indices;
                    size_t wOff = v * wStride;
                    outPrim.mesh.vertices[v].jointWeights = {wBase[wOff], wBase[wOff+1], wBase[wOff+2], wBase[wOff+3]};
                }
            }
            // indices
            const auto& idxAcc = model.accessors[primitive.indices];
            const auto& idxBv = model.bufferViews[idxAcc.bufferView];
            const unsigned char* idxData = &model.buffers[idxBv.buffer].data[idxBv.byteOffset + idxAcc.byteOffset];
            switch (idxAcc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                    const uint32_t* d = reinterpret_cast<const uint32_t*>(idxData);
                    outPrim.mesh.indices.assign(d, d + idxAcc.count);
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    const uint16_t* d = reinterpret_cast<const uint16_t*>(idxData);
                    for (size_t i = 0; i < idxAcc.count; ++i) outPrim.mesh.indices.push_back(d[i]);
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                    for (size_t i = 0; i < idxAcc.count; ++i) outPrim.mesh.indices.push_back(idxData[i]);
                    break;
                }
                default: std::fprintf(stderr, "[gltf] unsupported index type\n"); continue;
            }
            // Generate face normals when missing.
            bool hasNormals = false;
            for (const auto& v : outPrim.mesh.vertices) if (v.normal != glm::vec3{0.0f}) { hasNormals = true; break; }
            if (!hasNormals) {
                for (size_t i = 0; i + 2 < outPrim.mesh.indices.size(); i += 3) {
                    Vertex& a = outPrim.mesh.vertices[outPrim.mesh.indices[i]];
                    Vertex& b = outPrim.mesh.vertices[outPrim.mesh.indices[i + 1]];
                    Vertex& c = outPrim.mesh.vertices[outPrim.mesh.indices[i + 2]];
                    glm::vec3 n = glm::normalize(glm::cross(b.position - a.position, c.position - a.position));
                    a.normal = b.normal = c.normal = n;
                }
            }
            // material
            if (primitive.material >= 0 && primitive.material < (int)model.materials.size()) {
                const auto& mat = model.materials[primitive.material];
                const auto& pbr = mat.pbrMetallicRoughness;
                if (pbr.baseColorFactor.size() >= 3) {
                    outPrim.material.baseColor = {float(pbr.baseColorFactor[0]), float(pbr.baseColorFactor[1]), float(pbr.baseColorFactor[2]), pbr.baseColorFactor.size()>3 ? float(pbr.baseColorFactor[3]) : 1.0f};
                }
                outPrim.material.metallic = float(pbr.metallicFactor);
                outPrim.material.roughness = float(pbr.roughnessFactor);
                if (textures && pbr.baseColorTexture.index >=0 && pbr.baseColorTexture.index < (int)model.textures.size()) {
                    const auto& tex = model.textures[pbr.baseColorTexture.index];
                    if (tex.source >=0 && tex.source < (int)model.images.size()) {
                        const auto& image = model.images[tex.source];
                        std::string texKey = image.uri.empty() ? full + "#img" + std::to_string(tex.source) : image.uri;
                        std::string dir = full.substr(0, full.find_last_of('/')+1);
                        std::string texPath = image.uri.empty() ? texKey : dir + image.uri;
                        int tw, th, tc;
                        stbi_uc* pixels = stbi_load(texPath.c_str(), &tw, &th, &tc, STBI_rgb_alpha);
                        if (pixels) {
                            outPrim.material.baseColorTexture = textures->createFromPixels(texKey, pixels, tw, th);
                            stbi_image_free(pixels);
                        }
                    }
                }
            }
            std::printf("[gltf] %s primitive: %zu verts, %zu indices%s\n", full.c_str(), outPrim.mesh.vertices.size(), outPrim.mesh.indices.size(),
                        (jointsIt!=primitive.attributes.end() ? " [skinned]" : ""));
            outModel.primitives.push_back(std::move(outPrim));
        }
    }

    // --- skins -> skeletons ---
    // Build node parent map
    std::vector<int> nodeParent(model.nodes.size(), -1);
    for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
        for (int child : model.nodes[ni].children) {
            if (child >=0 && child < (int)nodeParent.size()) nodeParent[child] = (int)ni;
        }
    }
    for (const auto& skin : model.skins) {
        Skeleton skel;
        skel.joints.resize(skin.joints.size());
        std::unordered_map<int,int> nodeToJoint;
        for (size_t i=0;i<skin.joints.size();++i) nodeToJoint[skin.joints[i]] = (int)i;
        // inverse bind matrices
        std::vector<glm::mat4> invBinds(skin.joints.size(), glm::mat4(1.0f));
        if (skin.inverseBindMatrices >=0 && skin.inverseBindMatrices < (int)model.accessors.size()) {
            const auto& acc = model.accessors[skin.inverseBindMatrices];
            const auto& bv = model.bufferViews[acc.bufferView];
            const float* data = reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset + acc.byteOffset]);
            size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv)/sizeof(float) : 16;
            for (size_t i=0;i<skin.joints.size();++i) {
                glm::mat4 m(1.0f);
                // glTF stores column-major, glm is column-major, so direct copy
                for (int c=0;c<4;++c) for (int r=0;r<4;++r) m[c][r] = data[i*stride + c*4 + r];
                invBinds[i] = m;
            }
        }
        for (size_t i=0;i<skin.joints.size();++i) {
            int nodeIdx = skin.joints[i];
            const auto& node = model.nodes[nodeIdx];
            skel.joints[i].name = node.name;
            skel.joints[i].inverseBind = invBinds[i];
            int parentNode = nodeParent[nodeIdx];
            auto it = nodeToJoint.find(parentNode);
            skel.joints[i].parent = (it != nodeToJoint.end()) ? it->second : -1;
        }
        skel.resizePose();
        // Initialize pose from node transforms
        for (size_t i=0;i<skin.joints.size();++i) {
            int nodeIdx = skin.joints[i];
            const auto& node = model.nodes[nodeIdx];
            glm::vec3 t{0.0f}, s{1.0f};
            glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
            if (node.translation.size()==3) t = {float(node.translation[0]), float(node.translation[1]), float(node.translation[2])};
            if (node.scale.size()==3) s = {float(node.scale[0]), float(node.scale[1]), float(node.scale[2])};
            if (node.rotation.size()==4) r = glm::quat(float(node.rotation[3]), float(node.rotation[0]), float(node.rotation[1]), float(node.rotation[2]));
            if (!node.matrix.empty()) {
                glm::mat4 m(1.0f);
                for (int c=0;c<4;++c) for (int r2=0;r2<4;++r2) m[c][r2] = float(node.matrix[c*4+r2]);
                // Decompose matrix to TRS for pose - simple: extract translation, scale, rotation via glm::decompose
                // For now, if matrix present, ignore TRS and set pose to identity, but store matrix as local?
                // We'll decompose: translation = m[3], scale = length of basis, rotation from mat3
                t = glm::vec3(m[3]);
                glm::vec3 col0(m[0]), col1(m[1]), col2(m[2]);
                s = glm::vec3(glm::length(col0), glm::length(col1), glm::length(col2));
                glm::mat3 rm(col0/s.x, col1/s.y, col2/s.z);
                r = glm::quat_cast(rm);
            }
            skel.pose[i].translation = t;
            skel.pose[i].scale = s;
            skel.pose[i].rotation = r;
        }
        // Compute initial final matrices (bind pose)
        // Do a simple forward traversal to compute globals
        std::vector<glm::mat4> globals(skel.joints.size(), glm::mat4(1.0f));
        for (size_t i=0;i<skel.joints.size();++i) {
            glm::mat4 local = glm::translate(glm::mat4(1.0f), skel.pose[i].translation) * glm::mat4_cast(skel.pose[i].rotation) * glm::scale(glm::mat4(1.0f), skel.pose[i].scale);
            if (skel.joints[i].parent >=0) globals[i] = globals[skel.joints[i].parent] * local;
            else globals[i] = local;
            skel.finalMatrices[i] = globals[i] * skel.joints[i].inverseBind;
        }
        outModel.skeletons.push_back(std::move(skel));
    }

    // --- animations ---
    for (const auto& anim : model.animations) {
        Animation outAnim;
        outAnim.name = anim.name;
        // samplers
        for (const auto& sampler : anim.samplers) {
            AnimationSampler outSampler;
            outSampler.interpolation = sampler.interpolation;
            // inputs (times)
            {
                const auto& acc = model.accessors[sampler.input];
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data = reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset + acc.byteOffset]);
                size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv)/sizeof(float) : 1;
                outSampler.inputs.resize(acc.count);
                for (size_t i=0;i<acc.count;++i) outSampler.inputs[i] = data[i*stride];
                if (!outSampler.inputs.empty()) outAnim.duration = std::max(outAnim.duration, outSampler.inputs.back());
            }
            // outputs
            {
                const auto& acc = model.accessors[sampler.output];
                const auto& bv = model.bufferViews[acc.bufferView];
                const float* data = reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset + acc.byteOffset]);
                size_t stride = acc.ByteStride(bv) ? acc.ByteStride(bv)/sizeof(float) : 0;
                if (acc.type == TINYGLTF_TYPE_VEC3) {
                    if (stride==0) stride=3;
                    outSampler.outputs.resize(acc.count);
                    for (size_t i=0;i<acc.count;++i) outSampler.outputs[i] = glm::vec4(data[i*stride], data[i*stride+1], data[i*stride+2], 0.0f);
                } else if (acc.type == TINYGLTF_TYPE_VEC4) {
                    if (stride==0) stride=4;
                    outSampler.outputs.resize(acc.count);
                    for (size_t i=0;i<acc.count;++i) outSampler.outputs[i] = glm::vec4(data[i*stride], data[i*stride+1], data[i*stride+2], data[i*stride+3]);
                } else if (acc.type == TINYGLTF_TYPE_SCALAR) {
                    if (stride==0) stride=1;
                    outSampler.outputs.resize(acc.count);
                    for (size_t i=0;i<acc.count;++i) outSampler.outputs[i] = glm::vec4(data[i*stride], 0,0,0);
                }
            }
            outAnim.samplers.push_back(std::move(outSampler));
        }
        // channels
        for (const auto& channel : anim.channels) {
            AnimationChannel outCh;
            outCh.samplerIndex = channel.sampler;
            outCh.path = channel.target_path;
            int nodeIdx = channel.target_node;
            // Map node to joint if possible (find which skeleton contains this node)
            outCh.targetJoint = -1;
            if (!outModel.skeletons.empty()) {
                // Use first skeleton's mapping for now
                auto& skel = outModel.skeletons[0];
                // Build nodeToJoint for first skeleton
                // We need to find joint index for this node
                // skin.joints contains node indices, we can search
                for (size_t sj=0; sj<model.skins[0].joints.size(); ++sj) {
                    if (model.skins[0].joints[sj] == nodeIdx) { outCh.targetJoint = (int)sj; break; }
                }
            }
            // If not a joint, keep -1 (will be ignored for skeleton, but could animate non-skeleton nodes)
            outAnim.channels.push_back(std::move(outCh));
        }
        outModel.animations.push_back(std::move(outAnim));
    }

    if (outModel.primitives.empty()) {
        std::fprintf(stderr, "[gltf] no triangle primitives found in %s\n", full.c_str());
    } else {
        std::printf("[gltf] %s: %zu primitives, %zu skeletons, %zu animations\n", full.c_str(), outModel.primitives.size(), outModel.skeletons.size(), outModel.animations.size());
    }
    outModel.ok = true;
    return outModel;
}

std::vector<GLTFPrimitive> loadGLTF(const std::string& path, TextureCache* textures) {
    GLTFModel m = loadGLTFModel(path, textures);
    return m.primitives;
}

} // namespace engine
