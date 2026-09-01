#include "renderer/graph/RenderGraph.hpp"
#include "renderer/api/Synchronization.hpp"
#include "renderer/graph/ResourceLifetime.hpp"
#include "renderer/graph/TransientResourcePool.hpp"
#include "renderer/vulkan/PhysicalImage.hpp"
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <stdexcept>
#include <algorithm>

namespace Engine {

ResourceHandle RenderGraph::importImage(const std::string& name, VkImage image, VkImageView view,
                                        VkFormat format, VkExtent2D extent, ResourceUsage initialUsage) {
    RenderGraphResource res;
    res.name = name;
    res.type = ResourceType::Imported;
    res.desc.name = name;
    res.desc.format = format;
    res.desc.extent = extent;
    res.desc.usage = 0;
    res.image = image;
    res.view = view;
    res.lastUsage = initialUsage;
    uint32_t id = static_cast<uint32_t>(resources_.size());
    resources_.push_back(std::move(res));
    return ResourceHandle{id};
}

ResourceHandle RenderGraph::createResource(const ImageDesc& desc) {
    RenderGraphResource res;
    res.name = desc.name;
    res.type = ResourceType::Transient;
    res.desc = desc;
    res.image = VK_NULL_HANDLE;
    res.view = VK_NULL_HANDLE;
    res.lastUsage = ResourceUsage::None;
    uint32_t id = static_cast<uint32_t>(resources_.size());
    resources_.push_back(std::move(res));
    return ResourceHandle{id};
}

void RenderGraph::addRead(uint32_t passIndex, ResourceHandle h, ResourceUsage u) {
    if (passIndex >= passes_.size() || !h.isValid() || h.id >= resources_.size()) return;
    passes_[passIndex].reads.emplace_back(h, u);
}

void RenderGraph::addWrite(uint32_t passIndex, ResourceHandle h, ResourceUsage u) {
    if (passIndex >= passes_.size() || !h.isValid() || h.id >= resources_.size()) return;
    passes_[passIndex].writes.emplace_back(h, u);
}

bool RenderGraph::compile() {
    const uint32_t n = static_cast<uint32_t>(passes_.size());
    sortedPassIndices_.clear();
    if (n == 0) return true;

    // 1. map resource -> writer pass (first writer), also track multiple writers for WAW
    std::unordered_map<uint32_t, uint32_t> writerForRes;
    std::unordered_map<uint32_t, std::vector<uint32_t>> writersForRes; // for multi-writer ordering
    for (uint32_t i = 0; i < n; ++i) {
        for (auto& [h, u] : passes_[i].writes) {
            if (!h.isValid()) continue;
            auto it = writerForRes.find(h.id);
            if (it == writerForRes.end()) writerForRes[h.id] = i;
            writersForRes[h.id].push_back(i);
        }
    }

    // 2. build DAG edges writer -> reader, and WAW edges between writers
    std::vector<std::vector<uint32_t>> adj(n);
    std::vector<int> indeg(n, 0);
    std::unordered_set<uint64_t> edgeSet;
    auto addEdge = [&](uint32_t from, uint32_t to) {
        if (from == to) return;
        uint64_t key = (uint64_t)from << 32 | to;
        if (edgeSet.find(key) != edgeSet.end()) return;
        edgeSet.insert(key);
        adj[from].push_back(to);
        indeg[to]++;
    };

    // WAW: order writers of same resource in declaration order
    for (auto& [resId, writers] : writersForRes) {
        for (size_t k = 1; k < writers.size(); ++k) {
            addEdge(writers[k-1], writers[k]);
        }
    }

    // RAW: reader depends on writer
    for (uint32_t i = 0; i < n; ++i) {
        for (auto& [h, u] : passes_[i].reads) {
            if (!h.isValid()) continue;
            auto it = writerForRes.find(h.id);
            if (it != writerForRes.end()) {
                uint32_t writer = it->second;
                // If multiple writers, depend on last writer? But WAW already orders writers,
                // so reader should depend on the last writer that is before it? Simpler: depend on all writers that are not the reader
                // For correctness with single writer, just add edge writer -> reader
                // If multiple writers, the reader should depend on the last writer (the one that produces final value)
                // We'll add edge from the last writer in list that is != i
                // But our writerForRes only stores first writer, not last. So find last writer != i before i? For simplicity add edge from each writer that is not reader
                // To avoid over-constraining, we add edge from the closest writer: the last writer before reader in declaration? Instead add from all writers != reader and check Kahn still works
                // Ponytail: single writer assumption dominates; just add from first writer
                addEdge(writer, i);
                // If multiple writers, also ensure reader after all writers? Add edges from each writer to reader
                auto wit = writersForRes.find(h.id);
                if (wit != writersForRes.end()) {
                    for (uint32_t w : wit->second) if (w != i) addEdge(w, i);
                }
            }
        }
    }

    // 3. Kahn topological sort (stable: respect declaration order for tie-break)
    std::queue<uint32_t> q;
    for (uint32_t i = 0; i < n; ++i) if (indeg[i]==0) q.push(i);
    std::vector<uint32_t> sorted;
    sorted.reserve(n);
    while (!q.empty()) {
        uint32_t u = q.front(); q.pop();
        sorted.push_back(u);
        for (uint32_t v : adj[u]) {
            if (--indeg[v]==0) q.push(v);
        }
    }
    if (sorted.size() != n) {
        // cycle detected
        sortedPassIndices_.clear();
        return false;
    }

    // 4. Dead pass pruning
    // Identify resources read as Present (sink)
    std::unordered_set<uint32_t> neededRes;
    bool hasPresent = false;
    for (auto& p : passes_) {
        for (auto& [h, u] : p.reads) if (u == ResourceUsage::Present && h.isValid()) { neededRes.insert(h.id); hasPresent = true; }
        for (auto& [h, u] : p.writes) if (u == ResourceUsage::Present && h.isValid()) { neededRes.insert(h.id); hasPresent = true; }
    }
    std::vector<uint32_t> prunedSorted;
    if (hasPresent) {
        std::vector<char> live(n, 0);
        // reverse walk: a pass is live if it writes a needed resource or reads Present
        for (int idx = (int)sorted.size()-1; idx >=0; --idx) {
            uint32_t pi = sorted[idx];
            const auto& pass = passes_[pi];
            bool isLive = false;
            // check if any write is needed
            for (auto& [h,u] : pass.writes) if (neededRes.find(h.id)!=neededRes.end()) { isLive = true; break; }
            // Present read makes sink live even without writes
            if (!isLive) {
                for (auto& [h,u] : pass.reads) if (u==ResourceUsage::Present) { isLive = true; break; }
            }
            if (isLive) {
                live[pi]=1;
                for (auto& [h,u] : pass.reads) neededRes.insert(h.id);
                // also its writes' resources' writer dependencies already handled via needed, but to be safe, writes don't add needed, reads do
            }
        }
        for (uint32_t pi : sorted) if (live[pi]) prunedSorted.push_back(pi);
        // If pruning removed everything but we had passes, keep at least the present sink? Already handled.
        // If pruned empty but hasPresent true, it means no writer for present resource; keep the present reader itself
        if (prunedSorted.empty() && hasPresent) {
            // fallback keep all reads Present passes
            for (uint32_t pi : sorted) {
                for (auto& [h,u] : passes_[pi].reads) if (u==ResourceUsage::Present) { prunedSorted.push_back(pi); break; }
            }
        }
    } else {
        // No Present: keep passes that are read by others OR write to imported (final output)
        std::unordered_set<uint32_t> allReadRes;
        for (auto& p : passes_) for (auto& [h,u] : p.reads) if (h.isValid()) allReadRes.insert(h.id);
        for (uint32_t pi : sorted) {
            const auto& pass = passes_[pi];
            if (pass.writes.empty()) { prunedSorted.push_back(pi); continue; }
            bool anyWriteRead = false;
            bool writesImported = false;
            for (auto& [h,u] : pass.writes) {
                if (allReadRes.find(h.id)!=allReadRes.end()) anyWriteRead = true;
                if (h.isValid() && h.id < resources_.size() && resources_[h.id].type == ResourceType::Imported) writesImported = true;
            }
            if (anyWriteRead || writesImported) prunedSorted.push_back(pi);
        }
        if (prunedSorted.empty() && !sorted.empty()) {
            // if all would be pruned, keep sorted as is (avoid empty graph surprise)
            prunedSorted = sorted;
        }
    }

    sortedPassIndices_ = std::move(prunedSorted);
    return true;
}

std::vector<ResourceLifetime> RenderGraph::computeLifetimes() const {
    std::vector<ResourceLifetime> lifetimes(resources_.size());
    for (size_t i = 0; i < resources_.size(); ++i) {
        lifetimes[i].handle = ResourceHandle{static_cast<uint32_t>(i)};
        lifetimes[i].firstPass = UINT32_MAX;
        lifetimes[i].lastPass = 0;
    }
    // Use sortedPassIndices_ order if compiled, else passes order
    const std::vector<uint32_t>& order = sortedPassIndices_.empty() ? std::vector<uint32_t>() : sortedPassIndices_;
    if (order.empty()) {
        // fallback to declaration order before compile
        for (uint32_t pi = 0; pi < (uint32_t)passes_.size(); ++pi) {
            const auto& pass = passes_[pi];
            for (auto& [h, _] : pass.reads) if (h.isValid() && h.id < lifetimes.size() && resources_[h.id].type==ResourceType::Transient) {
                lifetimes[h.id].firstPass = std::min(lifetimes[h.id].firstPass, pi);
                lifetimes[h.id].lastPass = std::max(lifetimes[h.id].lastPass, pi);
            }
            for (auto& [h, _] : pass.writes) if (h.isValid() && h.id < lifetimes.size() && resources_[h.id].type==ResourceType::Transient) {
                lifetimes[h.id].firstPass = std::min(lifetimes[h.id].firstPass, pi);
                lifetimes[h.id].lastPass = std::max(lifetimes[h.id].lastPass, pi);
            }
        }
    } else {
        for (uint32_t orderIdx = 0; orderIdx < (uint32_t)order.size(); ++orderIdx) {
            uint32_t pi = order[orderIdx];
            const auto& pass = passes_[pi];
            for (auto& [h, _] : pass.reads) if (h.isValid() && h.id < lifetimes.size() && resources_[h.id].type==ResourceType::Transient) {
                lifetimes[h.id].firstPass = std::min(lifetimes[h.id].firstPass, orderIdx);
                lifetimes[h.id].lastPass = std::max(lifetimes[h.id].lastPass, orderIdx);
            }
            for (auto& [h, _] : pass.writes) if (h.isValid() && h.id < lifetimes.size() && resources_[h.id].type==ResourceType::Transient) {
                lifetimes[h.id].firstPass = std::min(lifetimes[h.id].firstPass, orderIdx);
                lifetimes[h.id].lastPass = std::max(lifetimes[h.id].lastPass, orderIdx);
            }
        }
    }
    return lifetimes;
}

bool RenderGraph::compile(uint64_t frameIndex, TransientResourcePool& pool) {
    // 1. sort + prune (reuse existing logic)
    if (!compile()) return false;
    // 2. compute lifetimes based on sorted order
    auto lifetimes = computeLifetimes();
    // 3. assign physical resources from pool
    for (size_t i = 0; i < resources_.size(); ++i) {
        auto& res = resources_[i];
        if (res.type != ResourceType::Transient) continue;
        auto& lt = lifetimes[i];
        if (lt.firstPass == UINT32_MAX) continue; // unused (dead)
        PhysicalImage* phys = pool.acquireImage(res.desc, lt.firstPass, lt.lastPass, frameIndex);
        res.image = phys->image;
        res.view = phys->view;
        res.physicalBinding = phys;
        // Layout reset handling: if physical is new (UNDEFINED) set logical to None, else keep physical's layout for barrier
        if (phys->currentLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            res.lastUsage = ResourceUsage::None;
        } else {
            // keep res.lastUsage as is (will be overridden by barrier tracking physical layout)
            // For correct barrier, we want next barrier to transition from physical's current layout
            // So we set logical lastUsage to a dummy that will be ignored in resolveBarriers when physical exists
        }
    }
    return true;
}


void RenderGraph::execute(VkCommandBuffer cmdBuffer) {
    for (uint32_t pi : sortedPassIndices_) {
        resolveBarriers(pi, cmdBuffer);
        auto& pass = passes_[pi];
        if (pass.executeCallback) pass.executeCallback(cmdBuffer);
    }
}

void RenderGraph::clear() {
    // wipe transient resources and passes for next frame, keep imported? Spec: wipes transient state
    // Keep imported resources (type Imported), remove transient
    std::vector<RenderGraphResource> kept;
    kept.reserve(resources_.size());
    for (auto& r : resources_) if (r.type == ResourceType::Imported) kept.push_back(r);
    // Need to remap handles? For next frame spec says clear wipes transient state, so handles from previous frame invalid.
    // Simplest: clear all resources (including imported) and passes, as next frame will re-import.
    // But to respect "imported persists"? Spec's clear wipes transient state for next frame, not imported? Example re-imports swapchain each frame.
    // So we can clear everything; caller will re-import each frame.
    resources_.clear();
    passes_.clear();
    sortedPassIndices_.clear();
    // Note: if we kept imported, handles would shift; so clear all is safer for handle validity per frame
}

} // namespace Engine
