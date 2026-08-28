#include "editor/anim_graph_editor.h"

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
    // Simple visible node: button + text (per-type color)
    ImVec2 size(150.0f, 78.0f);
    ImU32 col = n.type == "Blend" ? IM_COL32(90, 90, 180, 255)
              : n.type == "Param" ? IM_COL32(90, 160, 90, 255)
                                  : IM_COL32(160, 120, 60, 255);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col);
    if (ImGui::Button(n.type.c_str(), size)) selected_ = n.id;
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        n.y += ImGui::GetIO().MouseDelta.y;
    }
    // Pins: small circles for visual feedback
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetItemRectMin();
    float r = 6.0f;
    if (n.type != "Param") {
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
    if (n.type == "Blend") {
        for (int slot = 0; slot < 3; ++slot) {
            ImVec2 p = ImVec2(origin.x, origin.y + size.y * (slot == 2 ? 0.75f : (slot == 1 ? 0.65f : 0.35f)));
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
    ImGui::Text("Test Text - Editor Visible");

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
    EditorBuildResult built = buildRuntimeGraph(ed_, baseSkeleton);
    if (!built.error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", built.error.c_str());
    } else if (ImGui::Button("Apply to runtime")) {
        onApply(built.graph);
    }
    ImGui::End();
}

} // namespace engine
