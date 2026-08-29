#include "core/anim_graph_ai.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace engine {

namespace {

struct ClipSpec {
    const char* name;
    int clipIndex;
    float inMin, inMax; // blend range leading INTO this clip (from previous)
};

// Deterministic keyword scan: lowercased prompt, substring match.
std::vector<ClipSpec> parsePrompt(const std::string& prompt) {
    std::string p = prompt;
    std::transform(p.begin(), p.end(), p.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](const char* kw) { return p.find(kw) != std::string::npos; };

    std::vector<ClipSpec> clips;
    // Order matters: blend cascade idle -> walk -> run -> jump.
    bool idle = has("idle") || has("stand");
    bool walk = has("walk");
    bool run = has("run") || has("sprint");
    bool jump = has("jump") || has("leap");

    if (!idle && !walk && !run && !jump) {
        // Default locomotion per spec example.
        return {{"Idle", 0, 0.0f, 0.0f}, {"Walk", 1, 0.1f, 1.0f}, {"Run", 2, 1.5f, 2.5f}};
    }
    if (idle) clips.push_back({"Idle", 0, 0.0f, 0.0f});
    if (walk) clips.push_back({"Walk", 1, 0.1f, 1.0f});
    if (run)  clips.push_back({"Run", 2, 1.5f, 2.5f});
    if (jump) clips.push_back({"Jump", 3, 3.0f, 4.0f});
    // A lone non-idle clip still needs an idle base for the cascade.
    if (clips.front().name != std::string("Idle"))
        clips.insert(clips.begin(), {"Idle", 0, 0.0f, 0.0f});
    return clips;
}

std::string esc(const char* s) { return std::string(s); }

} // namespace

std::string generateGraphJSON(const std::string& prompt) {
    std::vector<ClipSpec> clips = parsePrompt(prompt);

    std::string j = "{\n  \"nodes\": [\n";
    // Node ids: 1 = Param Speed, 2.. = clips, then blends.
    // Layout: param right, clips left column, blends middle cascade.
    int paramId = 1;
    std::vector<int> clipIds(clips.size());
    int nextId = 2;
    for (size_t i = 0; i < clips.size(); ++i) clipIds[i] = nextId++;
    std::vector<int> blendIds;
    for (size_t i = 0; i + 1 < clips.size(); ++i) blendIds.push_back(nextId++);
    int outputId = blendIds.empty() ? clipIds[0] : blendIds.back();

    j += "    { \"id\": " + std::to_string(paramId) + ", \"type\": \"Param\", \"name\": \"Speed\", \"value\": 0.0, \"pos\": [480, 200] }";
    for (size_t i = 0; i < clips.size(); ++i) {
        j += ",\n    { \"id\": " + std::to_string(clipIds[i]) + ", \"type\": \"Clip\", \"name\": \"" + esc(clips[i].name) +
             "\", \"clip\": " + std::to_string(clips[i].clipIndex) +
             ", \"pos\": [40, " + std::to_string(40 + static_cast<int>(i) * 180) + "] }";
    }
    for (size_t i = 0; i < blendIds.size(); ++i) {
        const ClipSpec& to = clips[i + 1];
        j += ",\n    { \"id\": " + std::to_string(blendIds[i]) + ", \"type\": \"Blend\", \"inMin\": " +
             std::to_string(to.inMin) + ", \"inMax\": " + std::to_string(to.inMax) +
             ", \"pos\": [280, " + std::to_string(120 + static_cast<int>(i) * 160) + "] }";
    }
    j += "\n  ],\n  \"links\": [\n";

    std::vector<std::string> links;
    for (size_t i = 0; i < blendIds.size(); ++i) {
        // A input: previous blend (or first clip), B input: this clip.
        int aSrc = (i == 0) ? clipIds[0] : blendIds[i - 1];
        links.push_back("    { \"from\": " + std::to_string(aSrc) + ", \"to\": " + std::to_string(blendIds[i]) + ", \"toSlot\": 0 }");
        links.push_back("    { \"from\": " + std::to_string(clipIds[i + 1]) + ", \"to\": " + std::to_string(blendIds[i]) + ", \"toSlot\": 1 }");
        links.push_back("    { \"from\": " + std::to_string(paramId) + ", \"to\": " + std::to_string(blendIds[i]) + ", \"toSlot\": 2 }");
    }
    for (size_t i = 0; i < links.size(); ++i) {
        j += links[i];
        if (i + 1 < links.size()) j += ",\n";
    }
    j += "\n  ],\n  \"output\": " + std::to_string(outputId) + "\n}\n";
    return j;
}

} // namespace engine
