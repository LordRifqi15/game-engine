#define GLM_ENABLE_EXPERIMENTAL
#include "core/scene/SceneSerializer.hpp"
#include "core/scene/Scene.hpp"
#include "core/scene.h"

#include "ecs/components/TagComponent.hpp"
#include "core/transform_component.h"
#include "ecs/components/PhysicsComponent.hpp"
#include "ecs/components/ColliderComponent.hpp"
#include "ecs/components/BlackboardComponent.hpp"
#include "ecs/components/PathComponent.hpp"
#include "core/gameplay_component.h"
#include "core/gameplay_graph.h"
#include "core/anim_graph_asset.h"
#include "core/mesh_component.h"
#include "core/material_component.h"
#include "renderer/scene/LightComponent.hpp"
#include "core/skeleton.h"
#include "core/anim_state_machine.h"
#include "core/gltf_loader.h"
#include "renderer/vulkan/texture_cache.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/constants.hpp>

namespace Engine {

using json = nlohmann::json;

// Helpers for glm conversions
static glm::vec3 vec3FromJson(const json& arr, glm::vec3 def = glm::vec3(0.0f)) {
    if (!arr.is_array() || arr.size() < 3) return def;
    return glm::vec3(arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>());
}
static glm::vec4 vec4FromJson(const json& arr, glm::vec4 def = glm::vec4(1.0f)) {
    if (!arr.is_array() || arr.size() < 4) return def;
    return glm::vec4(arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>(),
                     arr[3].get<float>());
}
static json vec3ToJson(const glm::vec3& v) {
    return json::array({v.x, v.y, v.z});
}
static glm::quat quatFromJson(const json& arr) {
    if (!arr.is_array()) return glm::quat(1,0,0,0);
    if (arr.size() == 4) {
        // spec: [x,y,z,w]
        return glm::quat(arr[3].get<float>(), arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>());
    } else if (arr.size() == 3) {
        // euler vec3
        glm::vec3 e(arr[0].get<float>(), arr[1].get<float>(), arr[2].get<float>());
        return glm::quat(e);
    }
    return glm::quat(1,0,0,0);
}
static json quatToJson(const glm::quat& q) {
    return json::array({q.x, q.y, q.z, q.w});
}
static glm::vec3 eulerFromQuat(const glm::quat& q) {
    return glm::eulerAngles(q);
}

// Try to load a gameplay graph from a template file, with fallback
static std::shared_ptr<::engine::GameplayGraph> loadGameplayTemplate(const std::string& pathStr, const std::string& tag) {
    if (!pathStr.empty()) {
        // Try direct path and alternatives like engine.cpp does for anim graphs
        std::vector<std::string> candidates = {
            pathStr,
            std::string("assets/") + pathStr, // if path already has assets/ duplicate?
            pathStr // already
        };
        // Also try relative to build
        std::vector<std::string> alt = {
            pathStr,
            std::string("build/") + pathStr,
            std::string("../") + pathStr
        };
        for (auto& cand : alt) {
            ::engine::EditorGraph ed;
            if (::engine::loadGraph(ed, cand)) {
                auto g = ::engine::buildGameplayGraph(ed, nullptr);
                if (g) {
                    std::printf("[scene] loaded gameplay template %s (%zu nodes)\n", cand.c_str(), ed.nodes.size());
                    std::fflush(stdout);
                    return g;
                }
            }
        }
        std::printf("[scene] WARN: gameplay template not found '%s', using fallback for '%s'\n", pathStr.c_str(), tag.c_str());
        std::fflush(stdout);
    } else {
        std::printf("[scene] WARN: empty gameplay template for '%s', using fallback\n", tag.c_str());
        std::fflush(stdout);
    }
    // Fallback based on tag
    if (tag == "Player" || tag.find("Player") != std::string::npos) {
        return ::engine::GameplayGraph::makeMinimal(nullptr);
    } else if (tag.find("Guard") != std::string::npos || tag.find("NPC") != std::string::npos) {
        return ::engine::GameplayGraph::makeBlackboardChase(nullptr, 6.0f, 2.2f, 3.0f);
    } else {
        // Generic fallback: minimal
        return ::engine::GameplayGraph::makeMinimal(nullptr);
    }
}

bool SceneSerializer::serialize(const std::filesystem::path& filepath, ::engine::Registry& registry) {
    json root;
    root["scene"] = "Demo World";
    root["version"] = 1;
    root["entities"] = json::array();

    auto entities = registry.getAllEntities();
    // Sort for deterministic output
    std::sort(entities.begin(), entities.end());

    for (auto e : entities) {
        json ej;
        // Tag
        std::string tag = "Entity";
        if (auto* tc = registry.tryGetComponent<::Engine::TagComponent>(e)) {
            tag = tc->tag;
        } else if (auto* tc2 = registry.tryGetComponent<::engine::TagComponent>(e)) {
            tag = tc2->tag;
        }
        ej["tag"] = tag;

        // Transform
        if (auto* tr = registry.tryGetComponent<::engine::TransformComponent>(e)) {
            json t;
            t["position"] = vec3ToJson(tr->position);
            // Convert euler vec3 to quat for spec compliance
            glm::quat q = glm::quat(tr->rotation);
            t["rotation"] = quatToJson(q);
            t["scale"] = vec3ToJson(tr->scale);
            ej["transform"] = t;
        }

        // Physics
        if (auto* phys = registry.tryGetComponent<::Engine::PhysicsComponent>(e)) {
            json p;
            p["mass"] = phys->mass;
            p["linearDamping"] = phys->linearDamping;
            p["useGravity"] = phys->useGravity;
            // Also include velocity for roundtrip if non-zero
            if (glm::length(phys->velocity) > 0.001f) {
                p["velocity"] = vec3ToJson(phys->velocity);
            }
            ej["physics"] = p;
        }

        // Collider
        if (auto* col = registry.tryGetComponent<::Engine::ColliderComponent>(e)) {
            json c;
            c["type"] = (col->type == ::Engine::ColliderType::Sphere) ? "Sphere" : "AABB";
            c["radius"] = col->radius;
            // Also serialize halfExtents and centerOffset for symmetry if not default
            c["halfExtents"] = vec3ToJson(col->halfExtents);
            c["centerOffset"] = vec3ToJson(col->centerOffset);
            ej["collider"] = c;
        }

        // Blackboard
        if (auto* bb = registry.tryGetComponent<::Engine::BlackboardComponent>(e)) {
            json b;
            if (!bb->floats.empty()) {
                json f = json::object();
                for (auto& [k,v] : bb->floats) f[k] = v;
                b["floats"] = f;
            }
            if (!bb->bools.empty()) {
                json bl = json::object();
                for (auto& [k,v] : bb->bools) bl[k] = v;
                b["bools"] = bl;
            }
            if (!bb->vectors.empty()) {
                json vecs = json::object();
                for (auto& [k,v] : bb->vectors) vecs[k] = vec3ToJson(v);
                b["vectors"] = vecs;
            }
            if (!b.empty()) ej["blackboard"] = b;
        }

        // Path
        if (auto* path = registry.tryGetComponent<::Engine::PathComponent>(e)) {
            json pc;
            // Spec example has acceptanceRadius, but our component doesn't store it.
            // Write a default for spec compliance if path exists.
            pc["acceptanceRadius"] = 0.3;
            // Also write waypoints for roundtrip if present
            if (!path->waypoints.empty()) {
                json wps = json::array();
                for (auto& wp : path->waypoints) wps.push_back(vec3ToJson(wp));
                pc["waypoints"] = wps;
                pc["currentIndex"] = path->currentIndex;
                pc["hasPath"] = path->hasPath;
                pc["isFinished"] = path->isFinished;
                pc["destination"] = vec3ToJson(path->destination);
            }
            ej["path"] = pc;
        }

        // Gameplay
        if (auto* gc = registry.tryGetComponent<::engine::GameplayComponent>(e)) {
            if (gc->graph) {
                json g;
                // Infer template path from tag for roundtrip
                if (tag == "Player") g["graphTemplate"] = "assets/graphs/player_controller.json";
                else if (tag.find("Guard") != std::string::npos || tag.find("NPC") != std::string::npos) g["graphTemplate"] = "assets/graphs/npc_patrol_chase.json";
                else g["graphTemplate"] = "";
                // Check for targetTag via blackboard TargetEntityID
                if (auto* bb = registry.tryGetComponent<::Engine::BlackboardComponent>(e)) {
                    auto it = bb->floats.find("TargetEntityID");
                    if (it != bb->floats.end()) {
                        uint32_t targetId = static_cast<uint32_t>(it->second);
                        // Find tag for that entity
                        std::string targetTag = "";
                        for (auto other : entities) {
                            if (static_cast<uint32_t>(other) == targetId) {
                                if (auto* otc = registry.tryGetComponent<::Engine::TagComponent>(other)) {
                                    targetTag = otc->tag;
                                    break;
                                }
                            }
                        }
                        if (!targetTag.empty()) g["targetTag"] = targetTag;
                    }
                }
                // Only write gameplay if we have template or targetTag
                if (!g.empty()) ej["gameplay"] = g;
                else ej["gameplay"] = json::object();
            }
        }

        root["entities"].push_back(ej);
    }

    // Write to file
    std::filesystem::create_directories(filepath.parent_path());
    std::ofstream out(filepath);
    if (!out.is_open()) {
        std::fprintf(stderr, "[scene] Failed to open %s for writing\n", filepath.c_str());
        return false;
    }
    out << root.dump(2);
    out.close();
    std::printf("[scene] Serialized %zu entities to %s\n", entities.size(), filepath.c_str());
    std::fflush(stdout);
    return true;
}

bool SceneSerializer::deserialize(const std::filesystem::path& filepath, ::engine::Registry& registry,
                                  SceneAssets* assets) {
    std::ifstream in(filepath);
    if (!in.is_open()) {
        std::fprintf(stderr, "[scene] Failed to open %s for reading\n", filepath.c_str());
        return false;
    }
    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[scene] JSON parse error %s: %s\n", filepath.c_str(), e.what());
        return false;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) {
        std::fprintf(stderr, "[scene] Invalid scene JSON: missing entities array\n");
        return false;
    }

    // 1. Wipe current world entities
    registry.clear();

    std::unordered_map<std::string, ::engine::Entity> tagToEntityMap;

    // PASS 1: Entity Instantiation & Basic Components
    for (const auto& entityJson : root["entities"]) {
        ::engine::Entity entity = registry.createEntity();

        std::string tag = entityJson.value("tag", "Entity");
        registry.addComponent<::Engine::TagComponent>(entity, ::Engine::TagComponent(tag));
        tagToEntityMap[tag] = entity;

        // Transform
        if (entityJson.contains("transform")) {
            auto& tJson = entityJson["transform"];
            glm::vec3 pos(0.0f), scale(1.0f);
            glm::vec3 euler(0.0f);
            if (tJson.contains("position") && tJson["position"].is_array()) {
                pos = vec3FromJson(tJson["position"]);
            }
            if (tJson.contains("rotation") && tJson["rotation"].is_array()) {
                auto& r = tJson["rotation"];
                if (r.size() == 4) {
                    glm::quat q = quatFromJson(r);
                    euler = eulerFromQuat(q);
                } else if (r.size() == 3) {
                    euler = vec3FromJson(r);
                }
            }
            if (tJson.contains("scale") && tJson["scale"].is_array()) {
                scale = vec3FromJson(tJson["scale"], glm::vec3(1.0f));
            }
            ::engine::TransformComponent tc;
            tc.position = pos;
            tc.rotation = euler;
            tc.scale = scale;
            registry.addComponent<::engine::TransformComponent>(entity, tc);
        }

        // Physics
        if (entityJson.contains("physics")) {
            auto& pJson = entityJson["physics"];
            ::Engine::PhysicsComponent pc;
            pc.mass = pJson.value("mass", 1.0f);
            pc.linearDamping = pJson.value("linearDamping", 10.0f);
            pc.useGravity = pJson.value("useGravity", true);
            pc.isGrounded = pJson.value("isGrounded", false);
            if (pJson.contains("velocity") && pJson["velocity"].is_array()) {
                pc.velocity = vec3FromJson(pJson["velocity"]);
            } else {
                pc.velocity = glm::vec3(0.0f);
            }
            pc.acceleration = glm::vec3(0.0f);
            registry.addComponent<::Engine::PhysicsComponent>(entity, pc);
        }

        // Collider
        if (entityJson.contains("collider")) {
            auto& cJson = entityJson["collider"];
            ::Engine::ColliderComponent col;
            std::string typeStr = cJson.value("type", "Sphere");
            col.type = (typeStr == "Sphere") ? ::Engine::ColliderType::Sphere : ::Engine::ColliderType::AABB;
            col.radius = cJson.value("radius", 0.5f);
            if (cJson.contains("halfExtents") && cJson["halfExtents"].is_array()) {
                col.halfExtents = vec3FromJson(cJson["halfExtents"], glm::vec3(0.5f,1.0f,0.5f));
            }
            if (cJson.contains("centerOffset") && cJson["centerOffset"].is_array()) {
                col.centerOffset = vec3FromJson(cJson["centerOffset"], glm::vec3(0,0.5f,0));
            }
            registry.addComponent<::Engine::ColliderComponent>(entity, col);
        }

        // Mesh + material + skeleton (Task 053 parity scenes)
        if (assets && assets->meshes && entityJson.contains("mesh")) {
            auto& mJson = entityJson["mesh"];
            std::string gltf = mJson.value("gltf", "");
            uint32_t prim = mJson.value("primitive", 0);
            if (!gltf.empty()) {
                ::engine::GLTFModel model = ::engine::loadGLTFModel(gltf, assets->textures);
                if (model.ok && prim < model.primitives.size()) {
                    auto& p = model.primitives[prim];
                    assets->meshes->push_back(std::move(p.mesh));
                    ::engine::Mesh* meshPtr = &assets->meshes->back();
                    ::engine::MeshComponent mc;
                    mc.mesh = meshPtr;
                    registry.addComponent<::engine::MeshComponent>(entity, mc);
                    ::engine::MaterialComponent matC;
                    matC.material = p.material;
                    if (mJson.contains("material")) {
                        auto& oJson = mJson["material"];
                        if (oJson.contains("color") && oJson["color"].is_array()) {
                            matC.material.baseColor = vec4FromJson(oJson["color"]);
                        }
                        matC.material.metallic = oJson.value("metallic", matC.material.metallic);
                        matC.material.roughness = oJson.value("roughness", matC.material.roughness);
                        if (oJson.contains("texture") && assets->textures) {
                            const ::engine::Texture* tex = assets->textures->load(
                                oJson.value("texture", ""));
                            if (tex) matC.material.baseColorTexture = tex;
                        }
                    }
                    registry.addComponent<::engine::MaterialComponent>(entity, matC);
                    if (!model.skeletons.empty()) {
                        ::engine::SkeletonComponent skelC;
                        skelC.skeleton = model.skeletons[0];
                        registry.addComponent<::engine::SkeletonComponent>(entity, skelC);
                        if (!model.animations.empty()) {
                            ::engine::AnimationComponent animC;
                            animC.animations = model.animations;
                            animC.speed = 1.0f;
                            animC.playing = true;
                            animC.loop = true;
                            float dur = animC.animations.size() > 1
                                            ? animC.animations[1].duration
                                            : animC.animations[0].duration;
                            animC.machine = ::engine::makeDefaultStateMachine(
                                animC.animations, skelC.skeleton, dur > 0.0f ? dur : 1.0f);
                            registry.addComponent<::engine::AnimationComponent>(entity,
                                                                               std::move(animC));
                        }
                    }
                } else {
                    std::fprintf(stderr, "[scene] mesh load failed: %s (%s)\n", gltf.c_str(),
                                 model.error.c_str());
                }
            }
        }

        // Lights (point feeds clustering; directional overrides scene light)
        if (entityJson.contains("pointLight")) {
            auto& lJson = entityJson["pointLight"];
            ::Engine::PointLightComponent lc;
            if (lJson.contains("color") && lJson["color"].is_array()) {
                lc.color = vec3FromJson(lJson["color"], glm::vec3(1.0f));
            }
            lc.intensity = lJson.value("intensity", 10.0f);
            lc.radius = lJson.value("radius", 10.0f);
            if (lJson.contains("position") && lJson["position"].is_array()) {
                lc.position = vec3FromJson(lJson["position"]);
            }
            registry.addComponent<::Engine::PointLightComponent>(entity, lc);
        }
        if (entityJson.contains("directionalLight")) {
            auto& lJson = entityJson["directionalLight"];
            ::Engine::DirectionalLightComponent lc;
            if (lJson.contains("direction") && lJson["direction"].is_array()) {
                lc.direction = vec3FromJson(lJson["direction"], glm::vec3(0.0f, -1.0f, -1.0f));
            }
            if (lJson.contains("color") && lJson["color"].is_array()) {
                lc.color = vec3FromJson(lJson["color"], glm::vec3(1.0f));
            }
            lc.intensity = lJson.value("intensity", 1.0f);
            registry.addComponent<::Engine::DirectionalLightComponent>(entity, lc);
        }

        // Blackboard
        if (entityJson.contains("blackboard")) {
            auto& bbJson = entityJson["blackboard"];
            ::Engine::BlackboardComponent bb;
            if (bbJson.contains("floats") && bbJson["floats"].is_object()) {
                for (auto& [k,v] : bbJson["floats"].items()) {
                    if (v.is_number()) bb.setFloat(k, v.get<float>());
                }
            }
            if (bbJson.contains("bools") && bbJson["bools"].is_object()) {
                for (auto& [k,v] : bbJson["bools"].items()) {
                    if (v.is_boolean()) bb.setBool(k, v.get<bool>());
                }
            }
            if (bbJson.contains("vectors") && bbJson["vectors"].is_object()) {
                for (auto& [k,v] : bbJson["vectors"].items()) {
                    if (v.is_array() && v.size()>=3) bb.setVec3(k, vec3FromJson(v));
                }
            }
            // Also handle legacy where floats/bools may be directly under blackboard without wrapper?
            registry.addComponent<::Engine::BlackboardComponent>(entity, std::move(bb));
        }

        // Path
        if (entityJson.contains("path")) {
            ::Engine::PathComponent pc;
            auto& pathJson = entityJson["path"];
            // If waypoints present, restore
            if (pathJson.contains("waypoints") && pathJson["waypoints"].is_array()) {
                for (auto& wp : pathJson["waypoints"]) {
                    if (wp.is_array() && wp.size()>=3) pc.waypoints.push_back(vec3FromJson(wp));
                }
                pc.currentIndex = pathJson.value("currentIndex", size_t(0));
                pc.hasPath = pathJson.value("hasPath", false);
                pc.isFinished = pathJson.value("isFinished", false);
                if (pathJson.contains("destination") && pathJson["destination"].is_array()) {
                    pc.destination = vec3FromJson(pathJson["destination"]);
                }
            } else {
                // Empty path, just marker with acceptanceRadius (spec)
                pc.clear();
                // hasPath remains false, but component exists to indicate navigation
            }
            registry.addComponent<::Engine::PathComponent>(entity, std::move(pc));
        }
    }

    // PASS 2: External Asset Loading & Entity Reference Linking
    for (const auto& entityJson : root["entities"]) {
        std::string tag = entityJson.value("tag", "Entity");
        auto it = tagToEntityMap.find(tag);
        if (it == tagToEntityMap.end()) continue;
        ::engine::Entity entity = it->second;

        if (entityJson.contains("gameplay")) {
            auto& gpJson = entityJson["gameplay"];
            std::string graphPath = gpJson.value("graphTemplate", "");
            std::shared_ptr<::engine::GameplayGraph> graphInstance = loadGameplayTemplate(graphPath, tag);
            // Even if load fails, graphInstance will be fallback (non-null)
            if (graphInstance) {
                ::engine::GameplayComponent gc;
                gc.graph = graphInstance;
                registry.addComponent<::engine::GameplayComponent>(entity, std::move(gc));
            }

            // Target linking
            if (gpJson.contains("targetTag")) {
                std::string targetTag = gpJson["targetTag"].get<std::string>();
                auto targetIt = tagToEntityMap.find(targetTag);
                if (targetIt != tagToEntityMap.end()) {
                    ::engine::Entity targetEntity = targetIt->second;
                    // Ensure blackboard exists
                    ::Engine::BlackboardComponent* bb = registry.tryGetComponent<::Engine::BlackboardComponent>(entity);
                    if (!bb) {
                        ::Engine::BlackboardComponent newBb;
                        auto& ref = registry.addComponent<::Engine::BlackboardComponent>(entity, std::move(newBb));
                        bb = &ref;
                    }
                    bb->setFloat("TargetEntityID", static_cast<float>(static_cast<uint32_t>(targetEntity)));
                    // Also set generic target for debugging
                    std::printf("[scene] linked %s -> %s (id %u)\n", tag.c_str(), targetTag.c_str(), static_cast<uint32_t>(targetEntity));
                    std::fflush(stdout);
                } else {
                    std::printf("[scene] WARN: orphaned targetTag '%s' for entity '%s', setting 0\n", targetTag.c_str(), tag.c_str());
                    std::fflush(stdout);
                    // Ensure blackboard exists and set 0
                    ::Engine::BlackboardComponent* bb = registry.tryGetComponent<::Engine::BlackboardComponent>(entity);
                    if (!bb) {
                        ::Engine::BlackboardComponent newBb;
                        auto& ref = registry.addComponent<::Engine::BlackboardComponent>(entity, std::move(newBb));
                        bb = &ref;
                    }
                    bb->setFloat("TargetEntityID", 0.0f);
                }
            }
        }
    }

    std::printf("[scene] Deserialized %zu entities from %s\n", tagToEntityMap.size(), filepath.c_str());
    std::fflush(stdout);
    return true;
}

bool SceneSerializer::serialize(const std::filesystem::path& filepath, ::engine::Scene& scene) {
    return serialize(filepath, scene.registry());
}
bool SceneSerializer::deserialize(const std::filesystem::path& filepath, ::engine::Scene& scene) {
    return deserialize(filepath, scene.registry());
}

} // namespace Engine
