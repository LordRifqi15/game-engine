#include "core/anim_graph_asset.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace engine {

using json = nlohmann::json;

static json vec4ToJson(const glm::vec4& v) {
    return json::array({v.x, v.y, v.z, v.w});
}
static glm::vec4 jsonToVec4(const json& j) {
    return glm::vec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
}

static json samplerToJson(const AnimationSampler& s) {
    json j;
    j["interpolation"] = s.interpolation;
    j["inputs"] = s.inputs;
    json outs = json::array();
    for (auto& v : s.outputs) outs.push_back(vec4ToJson(v));
    j["outputs"] = outs;
    return j;
}
static AnimationSampler jsonToSampler(const json& j) {
    AnimationSampler s;
    s.interpolation = j.value("interpolation", "LINEAR");
    s.inputs = j.value("inputs", std::vector<float>{});
    if (j.contains("outputs")) {
        for (auto& e : j["outputs"]) s.outputs.push_back(jsonToVec4(e));
    }
    return s;
}

static json channelToJson(const AnimationChannel& c) {
    return json{{"targetJoint", c.targetJoint}, {"path", c.path}, {"samplerIndex", c.samplerIndex}};
}
static AnimationChannel jsonToChannel(const json& j) {
    AnimationChannel c;
    c.targetJoint = j.value("targetJoint", -1);
    c.path = j.value("path", "");
    c.samplerIndex = j.value("samplerIndex", -1);
    return c;
}

static json animationToJson(const Animation& a) {
    json j;
    j["name"] = a.name;
    j["duration"] = a.duration;
    json samps = json::array();
    for (auto& s : a.samplers) samps.push_back(samplerToJson(s));
    j["samplers"] = samps;
    json chans = json::array();
    for (auto& c : a.channels) chans.push_back(channelToJson(c));
    j["channels"] = chans;
    return j;
}
static Animation jsonToAnimation(const json& j) {
    Animation a;
    a.name = j.value("name", "");
    a.duration = j.value("duration", 0.0f);
    if (j.contains("samplers")) for (auto& e : j["samplers"]) a.samplers.push_back(jsonToSampler(e));
    if (j.contains("channels")) for (auto& e : j["channels"]) a.channels.push_back(jsonToChannel(e));
    return a;
}

bool saveGraph(const EditorGraph& graph, const std::string& path) {
    try {
        json j;
        json nodes = json::array();
        for (const auto& n : graph.nodes) {
            json nj;
            nj["id"] = n.id;
            nj["type"] = n.type;
            nj["pos"] = json::array({n.x, n.y});
            if (!n.name.empty()) nj["name"] = n.name;
            if (n.type == "Clip") {
                if (n.clipIndex >= 0) nj["clip"] = n.clipIndex;
                if (!n.clip.samplers.empty() || !n.clip.channels.empty() || !n.clip.name.empty()) {
                    nj["clipData"] = animationToJson(n.clip);
                }
            } else if (n.type == "Blend") {
                nj["inMin"] = n.inMin;
                nj["inMax"] = n.inMax;
                if (n.blendDuration != 0.3f) nj["blendDuration"] = n.blendDuration;
            } else if (n.type == "Param") {
                nj["value"] = n.value;
            }
            nodes.push_back(nj);
        }
        j["nodes"] = nodes;
        json links = json::array();
        for (auto& l : graph.links) {
            links.push_back(json{{"from", l.fromNode}, {"to", l.toNode}, {"toSlot", l.toSlot}});
        }
        j["links"] = links;
        j["output"] = graph.outputNode;

        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::ofstream f(path);
        if (!f) return false;
        f << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

bool loadGraphFromString(EditorGraph& outGraph, const std::string& jsonText) {
    try {
        json j = json::parse(jsonText);
        EditorGraph g;
        int maxId = 0;
        if (j.contains("nodes")) {
            for (auto& nj : j["nodes"]) {
                EditorNode n;
                n.id = nj.value("id", 0);
                n.type = nj.value("type", "");
                if (nj.contains("pos") && nj["pos"].is_array() && nj["pos"].size() >= 2) {
                    n.x = nj["pos"][0].get<float>();
                    n.y = nj["pos"][1].get<float>();
                }
                n.name = nj.value("name", "");
                if (n.type == "Clip") {
                    if (nj.contains("clip") && nj["clip"].is_number_integer()) {
                        n.clipIndex = nj["clip"].get<int>();
                    }
                    if (nj.contains("clipData") && nj["clipData"].is_object()) {
                        n.clip = jsonToAnimation(nj["clipData"]);
                    } else if (nj.contains("clip") && nj["clip"].is_object()) {
                        n.clip = jsonToAnimation(nj["clip"]);
                    }
                } else if (n.type == "Blend") {
                    n.inMin = nj.value("inMin", 0.0f);
                    n.inMax = nj.value("inMax", 1.0f);
                    n.blendDuration = nj.value("blendDuration", 0.3f);
                } else if (n.type == "Param") {
                    n.value = nj.value("value", 0.0f);
                }
                maxId = std::max(maxId, n.id);
                g.nodes.push_back(std::move(n));
            }
        }
        if (j.contains("links")) {
            for (auto& lj : j["links"]) {
                NodeLink l;
                l.fromNode = lj.value("from", -1);
                l.toNode = lj.value("to", -1);
                l.toSlot = lj.value("toSlot", 0);
                g.links.push_back(l);
            }
        }
        g.outputNode = j.value("output", -1);
        g.setNextId(maxId);
        outGraph = std::move(g);
        return true;
    } catch (...) {
        return false;
    }
}

bool loadGraph(EditorGraph& outGraph, const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadGraphFromString(outGraph, text);
}


std::shared_ptr<AnimGraph> loadGraphAsRuntime(const std::string& path, const Skeleton& baseSkeleton) {
    EditorGraph eg;
    if (!loadGraph(eg, path)) return nullptr;
    auto res = buildRuntimeGraph(eg, baseSkeleton);
    if (!res.graph) return nullptr;
    return res.graph;
}

std::shared_ptr<AnimGraph> loadGraphAsRuntime(const std::string& path, const Skeleton& baseSkeleton,
                                              const std::vector<Animation>& anims) {
    EditorGraph eg;
    if (!loadGraph(eg, path)) return nullptr;
    auto res = buildRuntimeGraph(eg, baseSkeleton, anims);
    if (!res.graph) return nullptr;
    return res.graph;
}

} // namespace engine
