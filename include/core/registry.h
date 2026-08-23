#pragma once

#include "core/component_array.h"
#include "core/entity.h"

#include <memory>
#include <unordered_map>
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

private:
    // Type-erased removal hook.
    struct IComponentArray {
        virtual ~IComponentArray() = default;
        virtual void removeEntity(Entity e) = 0;
    };

    template <typename T>
    struct ComponentArrayWrapper final : IComponentArray {
        ComponentArray<T> array;
        void removeEntity(Entity e) override { array.remove(e); }
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
