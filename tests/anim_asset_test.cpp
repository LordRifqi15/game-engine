// Test for Task 032 asset save/load round-trip
// Build: g++ -std=c++17 -I../include ../src/core/animation_system.cpp ../src/core/anim_graph.cpp ../src/core/anim_editor.cpp ../src/core/anim_graph_asset.cpp anim_asset_test.cpp -o anim_asset_test

#include "core/anim_graph_asset.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace engine;

static Animation makeClip(float angle, const std::string& name) {
    Animation a;
    a.name = name;
    a.duration = 1.0f;
    AnimationSampler s;
    s.interpolation = "LINEAR";
    glm::quat q = glm::angleAxis(angle, glm::vec3(0,1,0));
    s.inputs = {0.0f};
    s.outputs = {glm::vec4(q.x,q.y,q.z,q.w)};
    a.samplers.push_back(s);
    AnimationChannel ch; ch.targetJoint=0; ch.path="rotation"; ch.samplerIndex=0;
    a.channels.push_back(ch);
    return a;
}

int main() {
    Skeleton base;
    Joint j; j.parent=-1; base.joints={j}; base.resizePose();
    std::vector<Animation> anims{makeClip(0,"Idle"), makeClip(1.0f,"Walk")};

    EditorGraph g = makeLocomotionEditorGraph(anims);
    std::string path = "/tmp/test_locomotion.graph.json";
    if (!saveGraph(g, path)) { std::printf("FAIL save\n"); return 1; }
    if (!std::filesystem::exists(path)) { std::printf("FAIL file not exists\n"); return 1; }

    EditorGraph loaded;
    if (!loadGraph(loaded, path)) { std::printf("FAIL load\n"); return 1; }
    if (loaded.nodes.size() != g.nodes.size()) { std::printf("FAIL nodes size %zu vs %zu\n", loaded.nodes.size(), g.nodes.size()); return 1; }
    if (loaded.links.size() != g.links.size()) { std::printf("FAIL links size\n"); return 1; }
    if (loaded.outputNode != g.outputNode) { std::printf("FAIL output %d vs %d\n", loaded.outputNode, g.outputNode); return 1; }

    // Build runtime from both and compare pose at speed 0.5
    auto r1 = buildRuntimeGraph(g, base, anims);
    auto r2 = buildRuntimeGraph(loaded, base, anims);
    if (!r1.graph || !r2.graph) { std::printf("FAIL build\n"); return 1; }
    r1.graph->setSpeed(0.55f);
    r2.graph->setSpeed(0.55f);
    std::vector<Skeleton::Pose> p1, p2;
    for (int i=0;i<30;i++) { r1.graph->evaluate(1.0f/60.0f, p1); r2.graph->evaluate(1.0f/60.0f, p2); }
    if (p1.size()!=p2.size()|| p1.empty()) { std::printf("FAIL pose size\n"); return 1; }
    float yaw1 = 2*atan2(p1[0].rotation.y, p1[0].rotation.w);
    float yaw2 = 2*atan2(p2[0].rotation.y, p2[0].rotation.w);
    if (std::abs(yaw1-yaw2) > 0.01f) { std::printf("FAIL yaw %.3f vs %.3f\n", yaw1, yaw2); return 1; }

    // Test spec's minimal JSON: clip as integer
    std::string specPath = "/tmp/spec_minimal.json";
    {
        std::ofstream f(specPath);
        f << R"({"nodes":[{"id":1,"type":"Clip","clip":0,"pos":[100,200]},{"id":2,"type":"Clip","clip":1,"pos":[100,300]},{"id":3,"type":"Blend","inMin":0.1,"inMax":1.0,"pos":[300,250]}],"links":[{"from":1,"to":3,"toSlot":0},{"from":2,"to":3,"toSlot":1}],"output":3})";
    }
    EditorGraph specLoaded;
    if (!loadGraph(specLoaded, specPath)) { std::printf("FAIL spec load\n"); return 1; }
    if (specLoaded.nodes.size()!=3) { std::printf("FAIL spec nodes\n"); return 1; }
    // Build with anims
    auto r3 = buildRuntimeGraph(specLoaded, base, anims);
    if (!r3.graph) { std::printf("FAIL spec build %s\n", r3.error.c_str()); return 1; }

    std::printf("PASS: asset save/load round-trip, reload identical, spec minimal\n");
    return 0;
}
