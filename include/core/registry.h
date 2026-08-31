#pragma once

#include "core/component_array.h"
#include "core/entity.h"

#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>


namespace engine {

// Owns entity IDs + component arrays. Data only; logic lives in systems.
class Registry {
public:
    Entity createEntity() {
        return nextId_++;
    }

    void destroyEntity(Entity e) {
        // Remove from every registered component array.
        forEachArray([&](IComponentArray* arr) { arr->removeEntity(e); });
    }

    template <typename T>
    T& addComponent(Entity e, T component) {
        return arrayFor<T>().add(e, std::move(component));
    }

    template <typename T>
    T& getComponent(Entity e) {
        return arrayFor<T>().get(e);
    }

    template <typename T>
    const T& getComponent(Entity e) const {
        return const_cast<Registry*>(this)->arrayFor<T>().get(e);
    }

    template <typename T>
    T* tryGetComponent(Entity e) {
        return arrayFor<T>().tryGet(e);
    }

    template <typename T>
    bool hasComponent(Entity e) const {
        return const_cast<Registry*>(this)->arrayFor<T>().has(e);
    }

    template <typename T>
    ComponentArray<T>& array() {
        return arrayFor<T>();
    }

    // Returns nullptr if no entity has this component type yet.
    template <typename T>
    ComponentArray<T>* tryGetComponentArray() {
        const void* typeKey = &typeTag<T>;
        auto it = arrays_.find(typeKey);
        if (it == arrays_.end()) return nullptr;
        return &static_cast<ComponentArrayWrapper<T>*>(it->second.get())->array;
    }

    void clear() {
        for (auto& [k, v] : arrays_) {
            // Clear each component array via type-erased clear
            // We need to call clear on the underlying ComponentArray
            // Use virtual clear if available, otherwise reset via removal
            // For now, reset by clearing the wrapper's array directly
            // We add a virtual clear to IComponentArray
            v->clear();
        }
        arrays_.clear();
        nextId_ = 0;
    }

    std::vector<Entity> getAllEntities() const {
        std::unordered_map<Entity, bool> seen;
        std::vector<Entity> out;
        for (auto& [k, v] : arrays_) {
            v->collectEntities(seen, out);
        }
        return out;
    }

private:
    // Type-erased removal hook.
    struct IComponentArray {
        virtual ~IComponentArray() = default;
        virtual void removeEntity(Entity e) = 0;
        virtual void clear() = 0;
        virtual void collectEntities(std::unordered_map<Entity,bool>& seen, std::vector<Entity>& out) const = 0;
    };

    template <typename T>
    struct ComponentArrayWrapper final : IComponentArray {
        ComponentArray<T> array;
        void removeEntity(Entity e) override { array.remove(e); }
        void clear() override { array.clear(); }
        void collectEntities(std::unordered_map<Entity,bool>& seen, std::vector<Entity>& out) const override {
            for (size_t i=0;i<array.size();++i){
                Entity e = array.entityAt(i);
                if(!seen[e]){ seen[e]=true; out.push_back(e); }
            }
        }
    };

    template <typename Fn>
    void forEachArray(Fn&& fn) {
        for (auto& [typeId, arr] : arrays_) {
            fn(arr.get());
        }
    }


    template <typename T>
    ComponentArray<T>& arrayFor() {
        const void* typeKey = &typeTag<T>;
        auto it = arrays_.find(typeKey);
        if (it == arrays_.end()) {
            auto wrapper = std::make_unique<ComponentArrayWrapper<T>>();
            ComponentArray<T>* raw = &wrapper->array;
            arrays_.emplace(typeKey, std::move(wrapper));
            return *raw;
        }
        return static_cast<ComponentArrayWrapper<T>*>(it->second.get())->array;
    }

    template <typename T>
    static inline char typeTag; // address used as unique per-type key

    Entity nextId_ = 0;
    std::unordered_map<const void*, std::unique_ptr<IComponentArray>> arrays_;
};

} // namespace engine
