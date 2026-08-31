#pragma once

#include "core/entity.h"

#include <unordered_map>
#include <vector>

namespace engine {

// Sparse-set-flavored component storage: dense array for iteration,
// entity->index map for lookup. One ComponentArray per component type.
template <typename T>
class ComponentArray {
public:
    T& add(Entity e, T value) {
        auto [it, inserted] = index_.emplace(e, static_cast<uint32_t>(components_.size()));
        if (!inserted) {
            components_[it->second] = std::move(value);
            return components_[it->second];
        }
        entities_.push_back(e);
        return components_.emplace_back(std::move(value));
    }

    void remove(Entity e) {
        auto it = index_.find(e);
        if (it == index_.end()) return;

        uint32_t idx = it->second;
        uint32_t last = static_cast<uint32_t>(components_.size()) - 1;
        if (idx != last) {
            // Swap with last: keep the dense arrays packed.
            components_[idx] = std::move(components_[last]);
            entities_[idx] = entities_[last];
            index_[entities_[idx]] = idx;
        }
        components_.pop_back();
        entities_.pop_back();
        index_.erase(it);
    }

    T* tryGet(Entity e) {
        auto it = index_.find(e);
        return it == index_.end() ? nullptr : &components_[it->second];
    }
    const T* tryGet(Entity e) const {
        auto it = index_.find(e);
        return it == index_.end() ? nullptr : &components_[it->second];
    }

    T& get(Entity e) { return *tryGet(e); }
    const T& get(Entity e) const { return *tryGet(e); }

    bool has(Entity e) const { return index_.count(e) > 0; }

    void clear() {
        components_.clear();
        entities_.clear();
        index_.clear();
    }

    // Dense iteration.
    auto begin() { return components_.begin(); }
    auto end() { return components_.end(); }
    auto begin() const { return components_.cbegin(); }
    auto end() const { return components_.cend(); }

    size_t size() const { return components_.size(); }
    Entity entityAt(size_t i) const { return entities_[i]; }
    std::vector<T> components_;
    std::vector<Entity> entities_;
    std::unordered_map<Entity, uint32_t> index_;
};

} // namespace engine
