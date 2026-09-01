#pragma once
#include "renderer/meshlet/MeshletTypes.hpp"
#include "renderer/streaming/VirtualGeometryTypes.hpp"
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

namespace Engine {

// LOD DAG cluster tree definitions
struct MeshletHierarchyNode {
    uint32_t pageID{UINT32_MAX};
    uint32_t parentPageID{UINT32_MAX};
    uint32_t lodLevel{0};
    float lodError{0.0f};
    uint32_t meshletOffset{0};
    uint32_t meshletCount{0};
    glm::vec4 boundingSphere{0.0f}; // xyz center, w radius
};

class MeshletHierarchy {
public:
    MeshletHierarchy() = default;

    // Build hierarchy from flat meshlet list and page size (meshlets per page)
    void build(const std::vector<GPUMeshlet>& meshlets, uint32_t meshletsPerPage = 128);

    // LOD selection: for a given camera distance and error threshold, select pageID or fallback parent
    uint32_t selectLOD(uint32_t pageID, float distance, float threshold) const;

    // Accessors
    const std::vector<MeshletHierarchyNode>& nodes() const { return nodes_; }
    const std::vector<VirtualPageEntry>& virtualPages() const { return virtualPages_; }
    size_t pageCount() const { return nodes_.size(); }

    // Pin root pages (coarsest LOD) as permanently resident
    void pinRootPages(std::vector<VirtualPageEntry>& residencyTable) const {
        for (auto &n : nodes_) {
            if (n.parentPageID == UINT32_MAX && n.pageID < residencyTable.size()) {
                residencyTable[n.pageID].status = PAGE_STATUS_RESIDENT;
                // Keep physicalSlotIndex as is (assume 0 for root)
                if (residencyTable[n.pageID].physicalSlotIndex == UINT32_MAX) residencyTable[n.pageID].physicalSlotIndex = n.pageID;
            }
        }
    }

private:
    std::vector<MeshletHierarchyNode> nodes_;
    std::vector<VirtualPageEntry> virtualPages_;
};

inline void MeshletHierarchy::build(const std::vector<GPUMeshlet>& meshlets, uint32_t meshletsPerPage) {
    nodes_.clear();
    virtualPages_.clear();
    if (meshlets.empty() || meshletsPerPage==0) return;
    uint32_t pageCount = (static_cast<uint32_t>(meshlets.size()) + meshletsPerPage -1)/ meshletsPerPage;
    nodes_.reserve(pageCount);
    virtualPages_.reserve(pageCount);
    for(uint32_t p=0;p<pageCount;++p){
        MeshletHierarchyNode n;
        n.pageID = p;
        // Simple LOD DAG: quadtree parent = (p-1)/4, root 0 has no parent
        if(p == 0) n.parentPageID = UINT32_MAX;
        else n.parentPageID = (p - 1) / 4;
        n.lodLevel = (n.parentPageID==UINT32_MAX?0:1);
        n.lodError = float(p % 4) * 0.5f;
        n.meshletOffset = p * meshletsPerPage;
        n.meshletCount = std::min(meshletsPerPage, static_cast<uint32_t>(meshlets.size()) - n.meshletOffset);
        // Compute bounding sphere for page (average of meshlet spheres)
        glm::vec3 center(0); float maxR=0;
        for(uint32_t i=0;i<n.meshletCount;++i){
            auto &m = meshlets[n.meshletOffset + i];
            center += glm::vec3(m.boundingSphere);
        }
        if(n.meshletCount>0) center /= float(n.meshletCount);
        for(uint32_t i=0;i<n.meshletCount;++i){
            auto &m = meshlets[n.meshletOffset + i];
            float d = glm::length(glm::vec3(m.boundingSphere) - center) + m.boundingSphere.w;
            maxR = std::max(maxR, d);
        }
        n.boundingSphere = glm::vec4(center, maxR);
        nodes_.push_back(n);
        VirtualPageEntry e;
        e.physicalSlotIndex = UINT32_MAX;
        e.status = PAGE_STATUS_UNLOADED;
        e.meshletCount = n.meshletCount;
        e.parentPageID = n.parentPageID;
        virtualPages_.push_back(e);
    }
    // Pin roots will be done externally
}

inline uint32_t MeshletHierarchy::selectLOD(uint32_t pageID, float distance, float threshold) const {
    if(pageID >= nodes_.size()) return UINT32_MAX;
    const auto &node = nodes_[pageID];
    // Simple screen-space error: lodError * (100 / distance) < threshold => use this LOD, else fallback to parent
    float error = node.lodError * (100.0f / std::max(distance, 0.1f));
    if(error <= threshold) return pageID;
    if(node.parentPageID != UINT32_MAX) return selectLOD(node.parentPageID, distance, threshold);
    return pageID;
}


} // namespace Engine

namespace engine {
    using MeshletHierarchy = Engine::MeshletHierarchy;
    using MeshletHierarchyNode = Engine::MeshletHierarchyNode;
}
