#include "editor/anim_graph_editor.h"

#include "core/anim_graph_ai.h"
#include "core/anim_graph_asset.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace engine {

namespace {
ImVec2 pinPos(int slot, bool input, const ImVec2& origin, const ImVec2& size) {
    // Outputs on the right edge, inputs on the left edge, spread vertically.
    float fy = input ? (slot == 2 ? 0.75f : (slot == 1 ? 0.65f : 0.35f))
                     : 0.5f;
    return ImVec2(input ? origin.x : origin.x + size.x, origin.y + size.y * fy);
}
} // namespace
void AnimGraphEditor::drawNode(EditorNode& n) {
    ImVec2 pos(n.x, n.y);
    ImGui::SetCursorPos(pos);
    ImGui::PushID(n.id);
    ImVec2 size(150.0f, 78.0f);
    ImU32 col = IM_COL32(160, 120, 60, 255);
    if (n.type == "Blend" || n.type == "Branch") col = IM_COL32(90, 90, 180, 255);
    else if (n.type == "Param" || n.type == "Float") col = IM_COL32(90, 160, 90, 255);
    else if (n.type == "Input") col = IM_COL32(180, 90, 90, 255);
    else if (n.type == "SetParam") col = IM_COL32(180, 160, 90, 255);
    else if (n.type == "And" || n.type == "Timer") col = IM_COL32(120, 120, 180, 255);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    std::string label = n.type;
    if (!n.name.empty()) label += ": " + n.name;
    else if (n.type == "Input" && n.key) label += " (" + std::to_string(n.key) + ")";
    if (ImGui::Button(label.c_str(), size)) selected_ = n.id;
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        n.x += ImGui::GetIO().MouseDelta.x;
        n.y += ImGui::GetIO().MouseDelta.y;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetItemRectMin();
    float r = 6.0f;
    bool hasOutput = (n.type != "SetParam");
    if (hasOutput) {
        ImVec2 p = ImVec2(origin.x + size.x, origin.y + size.y * 0.5f);
        dl->AddCircleFilled(p, r, IM_COL32(220, 220, 120, 255));
        ImGui::SetCursorScreenPos(ImVec2(p.x - r, p.y - r));
        ImGui::InvisibleButton("out", ImVec2(r * 2, r * 2));
        if (ImGui::IsItemActive()) {
            dragging_ = true;
            dragFromNode_ = n.id;
            dragFromSlot_ = 0;
            dragX_ = ImGui::GetIO().MousePos.x;
            dragY_ = ImGui::GetIO().MousePos.y;
        }
    }
    int inputCount = 0;
    if (n.type == "Blend" || n.type == "Branch") inputCount = 3;
    else if (n.type == "And") inputCount = 2;
    else if (n.type == "SetParam") inputCount = 1;
    else if (n.type == "Float" || n.type == "Param" || n.type == "Input" || n.type == "Timer" || n.type == "Clip") inputCount = 0;
    for (int slot = 0; slot < inputCount; ++slot) {
        float fy = (inputCount == 1) ? 0.5f : (inputCount == 2 ? (slot == 0 ? 0.35f : 0.65f) : (slot == 2 ? 0.75f : (slot == 1 ? 0.65f : 0.35f)));
        ImVec2 p = ImVec2(origin.x, origin.y + size.y * fy);
        dl->AddCircleFilled(p, r, slot == 2 ? IM_COL32(120, 200, 120, 255) : IM_COL32(200, 200, 200, 255));
        ImGui::SetCursorScreenPos(ImVec2(p.x - r, p.y - r));
        ImGui::PushID(slot);
        ImGui::InvisibleButton("in", ImVec2(r * 2, r * 2));
        if (dragging_ && ImGui::IsItemHovered() && ImGui::IsMouseReleased(0)) {
            ed_.links.push_back({dragFromNode_, n.id, slot});
            dragging_ = false;
            dragFromNode_ = -1;
        }
        ImGui::PopID();
    }
    // Inline editors for selected node
    if (selected_ == n.id) {
        ImGui::SetCursorPos(ImVec2(pos.x, pos.y + size.y + 4));
        if (n.type == "Float" || n.type == "Param") {
            ImGui::PushID(n.id + 1000);
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("##val", &n.value, 0.05f);
            ImGui::PopID();
        } else if (n.type == "Input") {
            ImGui::PushID(n.id + 1000);
            ImGui::InputInt("##key", &n.key);
            ImGui::PopID();
        } else if (n.type == "SetParam") {
            ImGui::PushID(n.id + 1000);
            char buf[64]; std::snprintf(buf, sizeof(buf), "%s", n.targetParam.c_str());
            if (ImGui::InputText("##tgt", buf, sizeof(buf))) n.targetParam = buf;
            ImGui::PopID();
        } else if (n.type == "Blend" || n.type == "Branch") {
            ImGui::PushID(n.id + 1000);
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("##min", &n.inMin, 0.05f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::DragFloat("##max", &n.inMax, 0.05f);
            ImGui::PopID();
        }
    }
    ImGui::PopID();
}


void AnimGraphEditor::handleConnections() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Links: bezier from source output to target input.
    for (const auto& l : ed_.links) {
        EditorNode* from = ed_.find(l.fromNode);
        EditorNode* to = ed_.find(l.toNode);
        if (!from || !to) continue;
        ImVec2 p1 = pinPos(0, false, ImVec2(from->x, from->y), ImVec2(150.0f, 78.0f));
        ImVec2 p2 = pinPos(l.toSlot, true, ImVec2(to->x, to->y), ImVec2(150.0f, 78.0f));
        float dx = (p2.x - p1.x) * 0.5f;
        dl->AddBezierCubic(p1, ImVec2(p1.x + dx, p1.y), ImVec2(p2.x - dx, p2.y), p2,
                           IM_COL32(200, 200, 120, 255), 2.0f);
    }
    // In-progress drag line.
    if (dragging_) {
        EditorNode* from = ed_.find(dragFromNode_);
        if (from) {
            ImVec2 p1 = pinPos(0, false, ImVec2(from->x, from->y), ImVec2(150.0f, 78.0f));
            dl->AddLine(p1, ImVec2(dragX_, dragY_), IM_COL32(255, 255, 255, 180), 2.0f);
        }
        if (ImGui::IsMouseReleased(0)) dragging_ = false; // released nowhere
    }
}

void AnimGraphEditor::draw(const Skeleton& baseSkeleton,
                           const std::function<void(std::shared_ptr<AnimGraph>)>& onApply) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Graph Editor", &open)) {
        ImGui::End();
        return;
    }
    ImGui::Text("Editor Active: %zu nodes, output %d", ed_.nodes.size(), ed_.outputNode);

    // Task 033: prompt -> AI-generated graph.
    ImGui::SetNextItemWidth(360);
    ImGui::InputText("prompt", prompt_, sizeof(prompt_));
    ImGui::SameLine();
    if (ImGui::Button("Generate Graph")) {
        std::string json = generateGraphJSON(prompt_);
        EditorGraph gen;
        if (loadGraphFromString(gen, json)) {
            ed_ = std::move(gen);
            selected_ = -1;
            std::printf("[ai] generated graph for \"%s\": %zu nodes, output %d\n",
                        prompt_, ed_.nodes.size(), ed_.outputNode);
            std::fflush(stdout);
            // Auto-apply so the generated behavior runs immediately.
            EditorBuildResult r = anims_ ? buildRuntimeGraph(ed_, baseSkeleton, *anims_)
                                         : buildRuntimeGraph(ed_, baseSkeleton);
            if (r.graph) onApply(r.graph);
            else std::printf("[ai] generated graph build failed: %s\n", r.error.c_str());
        } else {
            std::printf("[ai] invalid JSON from generator (rejected)\n");
        }
        std::fflush(stdout);
    }
    ImGui::SameLine();

    // Toolbar: add nodes + apply.
    if (ImGui::Button("+ Clip")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "Clip"; n.name = "Clip";
        n.x = 60.0f; n.y = 60.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Blend")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "Blend";
        n.x = 300.0f; n.y = 200.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Param")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "Param"; n.name = "Param";
        n.x = 500.0f; n.y = 320.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Input")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "Input"; n.name = "W"; n.key = 87;
        n.x = 60.0f; n.y = 400.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Branch")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "Branch";
        n.x = 300.0f; n.y = 400.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ SetParam")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "SetParam"; n.targetParam = "speed";
        n.x = 500.0f; n.y = 400.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ And")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "And";
        n.x = 200.0f; n.y = 500.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Float")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "Float"; n.value = 1.0f;
        n.x = 350.0f; n.y = 500.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Timer")) {
        EditorNode& n = ed_.nodes.emplace_back();
        n.id = ed_.nextId(); n.type = "Timer";
        n.x = 500.0f; n.y = 500.0f;
    }
    ImGui::SameLine();

    if (selected_ != -1 && ImGui::Button("Delete")) {
        ed_.removeNode(selected_);
        selected_ = -1;
    }
    if (selected_ != -1) {
        ImGui::SameLine();
        ImGui::Text("selected #%d", selected_);
        // Click a link-end pair to delete: right-click a node removes its links.
        if (ImGui::IsMouseReleased(1)) {
            EditorNode* sel = ed_.find(selected_);
            if (sel) ed_.links.erase(std::remove_if(ed_.links.begin(), ed_.links.end(),
                                                    [&](const NodeLink& l) {
                                                        return l.fromNode == selected_ || l.toNode == selected_;
                                                    }),
                                     ed_.links.end());
        }
    }

    // Output marker: click "Set Output" per node body when selected.
    if (selected_ != -1 && ed_.find(selected_) && ImGui::SmallButton("Set Output")) {
        ed_.outputNode = selected_;
    }

    handleConnections();

    // Node canvas region.
    ImGui::BeginChild("canvas", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false);
    for (auto& n : ed_.nodes) drawNode(n);
    ImGui::EndChild();
    // Apply: editor graph -> runtime graph -> caller hook.
    EditorBuildResult built = anims_ ? buildRuntimeGraph(ed_, baseSkeleton, *anims_)
                                     : buildRuntimeGraph(ed_, baseSkeleton);

    if (!built.error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", built.error.c_str());
    } else {
        if (ImGui::Button("Apply to runtime")) {
            onApply(built.graph);
        }
        ImGui::SameLine();
        static char savePath[256] = "assets/animations/locomotion.graph.json";
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("##savepath", savePath, sizeof(savePath));
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            bool ok = saveGraph(ed_, savePath);
            // Also save to alternate cwd location for robustness (build/ vs root)
            std::string alt = std::string(savePath);
            if (alt.rfind("assets/", 0) == 0) {
                std::string buildAlt = std::string("build/") + alt;
                saveGraph(ed_, buildAlt);
            } else if (alt.rfind("build/assets/", 0) == 0) {
                std::string rootAlt = alt.substr(6);
                saveGraph(ed_, rootAlt);
            }
            if (ok) std::printf("[asset] saved %s (%zu nodes)\n", savePath, ed_.nodes.size());
            else std::printf("[asset] save failed %s\n", savePath);
            std::fflush(stdout);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            EditorGraph loaded;
            bool ok = loadGraph(loaded, savePath);
            if (!ok) {
                // Try alternate locations
                std::string alt = std::string(savePath);
                if (alt.rfind("assets/", 0) == 0) ok = loadGraph(loaded, std::string("build/") + alt);
                if (!ok && alt.rfind("build/assets/", 0) == 0) ok = loadGraph(loaded, alt.substr(6));
                if (!ok) ok = loadGraph(loaded, std::string("../") + savePath);
            }
            if (ok) {
                ed_ = std::move(loaded);
                std::printf("[asset] loaded %s (%zu nodes)\n", savePath, ed_.nodes.size());
            } else {
                std::printf("[asset] load failed %s\n", savePath);
            }
            std::fflush(stdout);
        }
    }
    ImGui::End();

} // namespace engine
}
