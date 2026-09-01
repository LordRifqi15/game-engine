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

    // EnTT-compatible aliases for SceneEditor
    Entity create() { return createEntity(); }
    void destroy(Entity e) { destroyEntity(e); }
    bool valid(Entity e) const {
        if (e == kInvalidEntity) return false;
        auto all = getAllEntities();
        for (auto ent : all) if (ent == e) return true;
        return false;
    }
    template <typename T>
    bool all_of(Entity e) const { return hasComponent<T>(e); }
    template <typename T, typename... Args>
    T& emplace(Entity e, Args&&... args) {
        return addComponent<T>(e, T{std::forward<Args>(args)...});
    }
    template <typename T>
    T& get(Entity e) { return getComponent<T>(e); }
    template <typename T>
    const T& get(Entity e) const { return getComponent<T>(e); }
    template <typename T>
    struct View {
        Registry* reg = nullptr;
        struct Iter {
            ComponentArray<T>* arr = nullptr;
            size_t idx = 0;
            Entity operator*() const { return arr ? arr->entityAt(idx) : kInvalidEntity; }
            bool operator!=(const Iter& o) const { return idx != o.idx; }
            Iter& operator++() { ++idx; return *this; }
        };
        Iter begin() {
            auto* arr = reg ? reg->template tryGetComponentArray<T>() : nullptr;
            if (!arr) return {nullptr, 0};
            return {arr, 0};
        }
        Iter end() {
            auto* arr = reg ? reg->template tryGetComponentArray<T>() : nullptr;
            if (!arr) return {nullptr, 0};
            return {arr, arr->size()};
        }
        template <typename U>
        U& get(Entity e) { return reg->template getComponent<U>(e); }
        template <typename U>
        const U& get(Entity e) const { return reg->template getComponent<U>(e); }
    };
    template <typename T>
    View<T> view() { return View<T>{this}; }
    template <typename T>
    View<T> view() const { return View<T>{const_cast<Registry*>(this)}; }

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

// EnTT compatibility alias for SceneEditor spec
namespace entt {
    using registry = ::engine::Registry;
    using entity = ::engine::Entity;
    constexpr ::engine::Entity null = ::engine::kInvalidEntity;
}
