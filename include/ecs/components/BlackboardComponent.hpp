#pragma once
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

namespace Engine {

struct BlackboardComponent {
    std::unordered_map<std::string, float> floats;
    std::unordered_map<std::string, bool> bools;
    std::unordered_map<std::string, glm::vec3> vectors;

    void setFloat(const std::string& key, float val) { floats[key] = val; }
    float getFloat(const std::string& key, float defaultVal = 0.0f) const {
        auto it = floats.find(key);
        return (it != floats.end()) ? it->second : defaultVal;
    }

    void setBool(const std::string& key, bool val) { bools[key] = val; }
    bool getBool(const std::string& key, bool defaultVal = false) const {
        auto it = bools.find(key);
        return (it != bools.end()) ? it->second : defaultVal;
    }

    void setVec3(const std::string& key, const glm::vec3& val) { vectors[key] = val; }
    glm::vec3 getVec3(const std::string& key, const glm::vec3& defaultVal = glm::vec3(0.0f)) const {
        auto it = vectors.find(key);
        return (it != vectors.end()) ? it->second : defaultVal;
    }

    bool hasKey(const std::string& key) const {
        return floats.count(key) || bools.count(key) || vectors.count(key);
    }
};

} // namespace Engine

namespace engine {
    using BlackboardComponent = ::Engine::BlackboardComponent;
}
