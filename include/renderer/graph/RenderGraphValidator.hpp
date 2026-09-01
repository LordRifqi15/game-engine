#pragma once
#include "renderer/graph/RenderGraph.hpp"
#include <string>

namespace Engine {

class RenderGraphValidator {
public:
    // Validates DAG for hazards: WAW, RAW, unconsumed outputs, uninitialized reads
    // Returns true if valid, false with error message
    static bool validate(const RenderGraph& graph, std::string& outError);

    // Convenience: assert and print
    static bool validateAndPrint(const RenderGraph& graph);
};

} // namespace Engine

namespace engine {
    using RenderGraphValidator = Engine::RenderGraphValidator;
}
