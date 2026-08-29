#pragma once

#include "core/anim_editor.h"

#include <memory>

struct ImGuiIO;

namespace engine {

// ImGui-based node editor (Task 031). Renders the EditorGraph; on Apply,
// converts it to a runtime AnimGraph and hands it to onApply.
class AnimGraphEditor {
public:
    explicit AnimGraphEditor(EditorGraph ed) : ed_(std::move(ed)) {}

    // Animations for resolving clip indices in AI-generated / loaded graphs.
    void setAnimations(const std::vector<Animation>* anims) { anims_ = anims; }

    // Call every frame while open. onApply receives rebuilt runtime graphs.
    void draw(const Skeleton& baseSkeleton,
              const std::function<void(std::shared_ptr<AnimGraph>)>& onApply);

    bool open = true;

private:
    void drawNode(EditorNode& n);
    void handleConnections();
    int pinAt(int nodeId, int slot, bool input); // helper for hit drawing

    EditorGraph ed_;
    const std::vector<Animation>* anims_ = nullptr;
    char prompt_[256] = "idle walk run with jump";
    int dragFromNode_ = -1;
    int dragFromSlot_ = -1;    // output slot index (Blend/Clip: 0)
    bool dragging_ = false;
    float dragX_ = 0.0f, dragY_ = 0.0f;
    int selected_ = -1;
};


} // namespace engine
