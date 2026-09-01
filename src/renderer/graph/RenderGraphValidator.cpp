#include "renderer/graph/RenderGraphValidator.hpp"
#include <unordered_set>
#include <unordered_map>
#include <cstdio>

namespace Engine {

bool RenderGraphValidator::validate(const RenderGraph& graph, std::string& outError) {
    const auto& resources = graph.resources();
    const auto& buffers = graph.bufferResources();
    const auto& passes = graph.passes();

    // Track for each image resource: first access type, write count, read count
    struct ResInfo { bool hasWrite=false; bool hasRead=false; bool firstIsRead=false; };
    std::unordered_map<uint32_t, ResInfo> imageInfo;
    std::unordered_map<uint32_t, ResInfo> bufferInfo;

    for (size_t pi = 0; pi < passes.size(); ++pi) {
        const auto& pass = passes[pi];
        for (auto& [h, u] : pass.reads) {
            if (!h.isValid() || h.id >= resources.size()) {
                outError = "Invalid image read handle in pass " + pass.name;
                return false;
            }
            auto &info = imageInfo[h.id];
            if (!info.hasWrite && !info.hasRead) info.firstIsRead = true;
            info.hasRead = true;
            // Check if transient and first access is read (uninitialized)
            if (resources[h.id].type == ResourceType::Transient && info.firstIsRead && !info.hasWrite) {
                // Allow reading depth that may be from previous frame? For now, flag as warning but not error if it's the first pass that reads it?
                // For strict validation, any transient read before write is hazard
                // Check if there was any prior write to this resource
                bool hasPriorWrite = false;
                for (size_t prev = 0; prev < pi; ++prev) {
                    for (auto& [wh, wu] : passes[prev].writes) if (wh.id == h.id) hasPriorWrite = true;
                }
                if (!hasPriorWrite) {
                    outError = "Uninitialized read of transient image '" + resources[h.id].name + "' in pass " + pass.name;
                    return false;
                }
            }
        }
        for (auto& [h, u] : pass.writes) {
            if (!h.isValid() || h.id >= resources.size()) {
                outError = "Invalid image write handle in pass " + pass.name;
                return false;
            }
            auto &info = imageInfo[h.id];
            info.hasWrite = true;
        }
        for (auto& [h, u] : pass.bufferReads) {
            if (!h.isValid() || h.id >= buffers.size()) {
                outError = "Invalid buffer read handle in pass " + pass.name;
                return false;
            }
            auto &info = bufferInfo[h.id];
            if (!info.hasWrite && !info.hasRead) info.firstIsRead = true;
            info.hasRead = true;
            if (buffers[h.id].type == ResourceType::Transient && info.firstIsRead && !info.hasWrite) {
                bool hasPriorWrite = false;
                for (size_t prev = 0; prev < pi; ++prev) {
                    for (auto& [wh, wu] : passes[prev].bufferWrites) if (wh.id == h.id) hasPriorWrite = true;
                }
                if (!hasPriorWrite) {
                    outError = "Uninitialized read of transient buffer '" + buffers[h.id].name + "' in pass " + pass.name;
                    return false;
                }
            }
        }
        for (auto& [h, u] : pass.bufferWrites) {
            if (!h.isValid() || h.id >= buffers.size()) {
                outError = "Invalid buffer write handle in pass " + pass.name;
                return false;
            }
            bufferInfo[h.id].hasWrite = true;
        }
    }

    // Check for unconsumed transient outputs (WAW / dead code)
    // A transient that is written but never read and not an imported output is unconsumed
    // For this check, we consider any transient write that has no subsequent read as potential dead code
    // But we allow it if the graph has a Present or swapchain write (final output) - those are typically imported writes
    // For strictness, we flag any transient with hasWrite && !hasRead as warning, but for validator we return false only if it's clearly dead and not an output
    // To avoid false positives for GBuffer that is read by later passes, we already tracked hasRead, so only truly unconsumed will be flagged
    for (auto& [id, info] : imageInfo) {
        if (id >= resources.size()) continue;
        const auto& res = resources[id];
        if (res.type == ResourceType::Transient && info.hasWrite && !info.hasRead) {
            // Check if this resource is written as final output that is not read but is intended (e.g., HDR that is then read by PostProcess)
            // Since we track hasRead, if it's never read, it's dead
            // Exception: if the resource is the swapchain imported, it's not transient, so not here
            outError = "Unconsumed transient image '" + res.name + "' written but never read (WAW/dead code)";
            return false;
        }
    }
    for (auto& [id, info] : bufferInfo) {
        if (id >= buffers.size()) continue;
        const auto& res = buffers[id];
        if (res.type == ResourceType::Transient && info.hasWrite && !info.hasRead) {
            outError = "Unconsumed transient buffer '" + res.name + "' written but never read";
            return false;
        }
    }

    // Check for WAW hazards: multiple writes to same resource without ordering handled by DAG
    // Our DAG already inserts WAW edges, so this is not a hazard if compile succeeds. We just ensure no two writes in same pass (obviously)
    // For validator, we just ensure compile would succeed (no cycles) - caller should check graph.compile() separately

    outError.clear();
    return true;
}

bool RenderGraphValidator::validateAndPrint(const RenderGraph& graph) {
    std::string err;
    bool ok = validate(graph, err);
    if (!ok) {
        std::fprintf(stderr, "RenderGraphValidator failed: %s\n", err.c_str());
    }
    return ok;
}

} // namespace Engine
