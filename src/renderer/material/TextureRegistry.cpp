#include "renderer/material/TextureRegistry.hpp"
#include "renderer/material/BindlessDescriptorManager.hpp"

namespace Engine {

void TextureRegistry::init(VkDevice device, BindlessDescriptorManager* manager) {
    device_ = device;
    manager_ = manager;
    freeSlots_.clear();
    slotToView_.clear();
    nextSlot_ = RESERVED_SLOTS;
    allocatedCount_ = 0;
    hasFallbacks_ = false;
    // Reserve 0-3 via manager (manager already has them, but ensure registry tracks)
    ensureFallbacks();
}

void TextureRegistry::shutdown() {
    freeSlots_.clear();
    slotToView_.clear();
    nextSlot_ = RESERVED_SLOTS;
    allocatedCount_ = 0;
    hasFallbacks_ = false;
    manager_ = nullptr;
    device_ = VK_NULL_HANDLE;
}

void TextureRegistry::ensureFallbacks() {
    if (hasFallbacks_) return;
    // Reserve 0-3 even without manager (headless)
    for (uint32_t i = 0; i < RESERVED_SLOTS; ++i) {
        slotToView_[i] = reinterpret_cast<VkImageView>(0x100 + i);
    }
    allocatedCount_ = RESERVED_SLOTS;
    hasFallbacks_ = true;
    // If manager exists, its slots are already reserved, but we keep registry in sync
}

uint32_t TextureRegistry::allocate(VkImageView imageView) {
    if (imageView == VK_NULL_HANDLE) {
        return fallbackWhiteSlot(); // fallback
    }
    if (!manager_) {
        // Headless without manager: simulate slot allocation
        uint32_t slot;
        if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        } else {
            if (nextSlot_ >= BindlessDescriptorManager::MAX_BINDLESS_TEXTURES) return INVALID_SLOT;
            slot = nextSlot_++;
        }
        slotToView_[slot] = imageView;
        allocatedCount_++;
        return slot;
    }
    uint32_t slot = manager_->registerTexture(imageView);
    slotToView_[slot] = imageView;
    // manager already tracks allocated, but we mirror count
    if (slot >= nextSlot_) nextSlot_ = slot + 1;
    allocatedCount_ = static_cast<uint32_t>(slotToView_.size());
    return slot;
}

void TextureRegistry::free(uint32_t slot) {
    if (slot < RESERVED_SLOTS) return; // never free fallbacks
    auto it = slotToView_.find(slot);
    if (it == slotToView_.end()) return;
    slotToView_.erase(it);
    freeSlots_.push_back(slot);
    if (allocatedCount_ > 0) allocatedCount_--;
    if (manager_) {
        manager_->freeTexture(slot);
    }
}

bool TextureRegistry::isValid(uint32_t slot) const {
    if (slot >= BindlessDescriptorManager::MAX_BINDLESS_TEXTURES) return false;
    auto it = slotToView_.find(slot);
    return it != slotToView_.end() && it->second != VK_NULL_HANDLE;
}

} // namespace Engine
