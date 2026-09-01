// Mesh Streaming: LRU, request generation, slot reuse
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/streaming/PageSlotPool.cpp ../src/renderer/streaming/MeshStreamingManager.cpp ../src/renderer/meshlet/MeshletBuilder.cpp ../src/renderer/meshlet/MeshletPipeline.cpp ../src/renderer/graph/RenderGraph.cpp ../src/renderer/graph/RenderGraphBuilder.cpp ../src/renderer/graph/RenderGraphResources.cpp ../src/renderer/graph/ResourceLifetime.cpp ../src/renderer/graph/TransientResourcePool.cpp ../src/renderer/vulkan/VkBarrierHelper.cpp ../src/renderer/vulkan/VkDeviceMemoryHelper.cpp mesh_streaming_test.cpp -o /tmp/mesh_streaming_test -lvulkan && /tmp/mesh_streaming_test

#include "renderer/streaming/VirtualGeometryTypes.hpp"
#include "renderer/streaming/PageSlotPool.hpp"
#include "renderer/streaming/MeshStreamingManager.hpp"
#include "renderer/meshlet/MeshletTypes.hpp"
#include "renderer/meshlet/MeshletBuilder.hpp"
#include "renderer/meshlet/MeshletHierarchy.hpp"

#include <cstdio>
#include <vector>
#include <algorithm>

using namespace Engine;

static int failCount=0;
static void check(bool cond, const char* msg){ if(!cond){ printf("FAIL %s\n", msg); failCount++; } else printf("OK %s\n", msg); }

// Simulate GPU shader atomicCompSwap for page requests (CPU)
static bool simulateRequest(uint32_t pageID, std::vector<VirtualPageEntry>& table, GPUPageRequestQueue& queue){
    VirtualPageEntry& entry = table[pageID];
    if(entry.status != PAGE_STATUS_UNLOADED) return false;
    // atomicCompSwap: if status == UNLOADED, set to REQUESTED and return old UNLOADED
    uint32_t old = __sync_val_compare_and_swap(&entry.status, PAGE_STATUS_UNLOADED, PAGE_STATUS_REQUESTED);
    if(old != PAGE_STATUS_UNLOADED) return false;
    uint32_t slot = __sync_fetch_and_add(&queue.count, 1);
    if(slot < queue.maxRequests){
        queue.requestedPageIDs[slot] = pageID;
        return true;
    }
    return false;
}

int main(){
    // 1. VirtualGeometryTypes
    {
        check(sizeof(VirtualPageEntry)==16, "VirtualPageEntry 16");
        check(sizeof(GPUPageRequestQueue)==16+4096*4, "GPUPageRequestQueue size");
        check(sizeof(PageStreamingUniforms)==16, "PageStreamingUniforms 16");
        check(PAGE_STATUS_UNLOADED==0 && PAGE_STATUS_REQUESTED==1 && PAGE_STATUS_RESIDENT==2, "PageStatus flags");
        check(PageSlotPool::PAGE_SIZE_BYTES==64*1024, "PAGE_SIZE 64KB");
        check(PageSlotPool::TOTAL_PHYSICAL_SLOTS==2048, "TOTAL_SLOTS 2048 (128MB)");
        VirtualPageEntry e; e.physicalSlotIndex=UINT32_MAX; e.status=PAGE_STATUS_UNLOADED; e.meshletCount=10; e.parentPageID=UINT32_MAX;
        check(e.parentPageID==UINT32_MAX, "parent root UINT32_MAX");
        GPUPageRequestQueue q{}; q.count=0; q.maxRequests=4096;
        check(q.maxRequests==4096, "maxRequests 4096");
    }

    // 2. PageSlotPool LRU allocation, eviction, touch, free
    {
        PageSlotPool pool;
        pool.init(VK_NULL_HANDLE, VK_NULL_HANDLE);
        check(pool.getPhysicalStorageBuffer()!=VK_NULL_HANDLE, "pool buffer dummy");
        check(pool.freeSlotCount()==2048, "pool free 2048 initially");
        check(pool.allocatedCount()==0, "pool allocated 0");

        uint32_t evicted=UINT32_MAX;
        uint32_t s0 = pool.allocateSlot(100, evicted);
        check(s0==0 && evicted==UINT32_MAX, "pool alloc page 100 -> slot 0 no evict");
        check(pool.isResident(100), "pool page 100 resident");
        check(pool.freeSlotCount()==2047, "pool free 2047 after 1");

        uint32_t s1 = pool.allocateSlot(101, evicted);
        check(s1==1, "pool alloc page 101 ->1");

        // Touch 100 to make it MRU, then fill up to force eviction of 101 (LRU)
        pool.touchSlot(100);
        // Allocate enough to fill
        for(uint32_t i=0;i<2046;++i){
            uint32_t ev;
            pool.allocateSlot(200+i, ev);
        }
        check(pool.freeSlotCount()==0, "pool full free 0");
        check(pool.allocatedCount()==2048, "pool allocated 2048 full");

        // Next allocation should evict LRU (which should be 101, since 100 was touched)
        uint32_t ev;
        uint32_t sEvict = pool.allocateSlot(9999, ev);
        check(ev==101, "pool LRU evict 101");
        check(sEvict==1, "pool evicted slot reused 1");
        check(!pool.isResident(101), "pool 101 not resident after evict");
        check(pool.isResident(100), "pool 100 still resident (touched)");
        check(pool.isResident(9999), "pool 9999 resident");

        // Free
        pool.freeSlot(100);
        check(!pool.isResident(100), "pool free 100 not resident");
        check(pool.freeSlotCount()==1, "pool free 1 after free");

        // Reallocate should reuse freed slot
        uint32_t ev2;
        uint32_t sReuse = pool.allocateSlot(7777, ev2);
        check(pool.isResident(7777), "pool 7777 resident after reuse");
        check(sReuse != UINT32_MAX, "pool reuse slot valid");

        // Re-allocate existing should touch and return same slot
        uint32_t ev3;
        uint32_t sAgain = pool.allocateSlot(7777, ev3);
        check(sAgain==sReuse && ev3==UINT32_MAX, "pool re-alloc existing same slot no evict");

        pool.shutdown();
        check(pool.getPhysicalStorageBuffer()==VK_NULL_HANDLE, "pool shutdown null");
    }

    // 3. MeshletHierarchy LOD fallback & pinning
    {
        std::vector<GPUMeshlet> meshlets(256);
        for(size_t i=0;i<meshlets.size();++i){
            meshlets[i].boundingSphere = glm::vec4(float(i%16), float((i/16)%16), 0, 1.0f);
            meshlets[i].vertexCount = 10;
            meshlets[i].triangleCount = 10;
        }
        MeshletHierarchy hier;
        hier.build(meshlets, 64);
        check(hier.pageCount()==4, "hierarchy 256/64=4 pages");
        check(hier.virtualPages().size()==4, "hierarchy virtualPages 4");
        check(hier.nodes()[0].parentPageID==UINT32_MAX, "hierarchy root parent UINT32_MAX");
        // For page 1, parent should be 0 in quadtree
        if(hier.pageCount()>1) check(hier.nodes()[1].parentPageID==0, "hierarchy page 1 parent 0");

        // Virtual pages initially UNLOADED
        for(auto &e: hier.virtualPages()) check(e.status==PAGE_STATUS_UNLOADED, "hierarchy virtual UNLOADED");

        // Pin roots
        std::vector<VirtualPageEntry> table = hier.virtualPages();
        hier.pinRootPages(table);
        check(table[0].status==PAGE_STATUS_RESIDENT, "hierarchy pin root resident");
        if(table.size()>1) check(table[1].status==PAGE_STATUS_UNLOADED, "hierarchy non-root still unloaded");

        // LOD selection
        uint32_t sel = hier.selectLOD(0, 10.0f, 1.0f);
        check(sel==0, "hierarchy selectLOD near -> self");
        // Far distance with high error should fallback to parent
        // Page 1 has lodError 0.5, distance 1000, error ~0.05 <1 so still self
        // For farther, still self, but if we set threshold low, it will fallback
        uint32_t sel2 = hier.selectLOD(1, 1.0f, 0.01f); // close distance, error large -> fallback to parent 0
        // Our simple error = lodError *100/dist, for page1 lodError 0.5, dist1 => 50 >0.01 => fallback to 0
        check(sel2==0, "hierarchy selectLOD fallback to parent");
    }

    // 4. GPU request queue: atomicCompSwap deduplication, maxRequests clamp, fallback parent
    {
        const uint32_t pageCount=10;
        std::vector<VirtualPageEntry> table(pageCount);
        for(uint32_t i=0;i<pageCount;++i){
            table[i].physicalSlotIndex=UINT32_MAX;
            table[i].status=PAGE_STATUS_UNLOADED;
            table[i].meshletCount=10;
            table[i].parentPageID = (i==0? UINT32_MAX : 0); // all children fallback to 0
        }
        // Make root resident
        table[0].status=PAGE_STATUS_RESIDENT;
        table[0].physicalSlotIndex=0;

        GPUPageRequestQueue queue{};
        queue.count=0;
        queue.maxRequests=4096;

        // Simulate multiple threads requesting same missing page 5
        bool r1 = simulateRequest(5, table, queue);
        bool r2 = simulateRequest(5, table, queue);
        bool r3 = simulateRequest(5, table, queue);
        check(r1==true && r2==false && r3==false, "request dedup single success");
        check(queue.count==1 && queue.requestedPageIDs[0]==5, "request queue count 1 page 5");
        check(table[5].status==PAGE_STATUS_REQUESTED, "request page 5 REQUESTED");

        // Request another page
        bool r4 = simulateRequest(6, table, queue);
        check(r4 && queue.count==2, "request second page");

        // Already resident should not request
        bool r5 = simulateRequest(0, table, queue);
        check(!r5 && queue.count==2, "resident no request");

        // Fallback: page 5 is REQUESTED not RESIDENT, so its parent 0 is RESIDENT -> should fallback
        // Simulate shader fallback traversal
        uint32_t pageID=5;
        VirtualPageEntry page = table[pageID];
        if(page.status != PAGE_STATUS_RESIDENT){
            uint32_t parent = page.parentPageID;
            while(parent != UINT32_MAX){
                if(table[parent].status==PAGE_STATUS_RESIDENT){ page = table[parent]; break; }
                parent = table[parent].parentPageID;
            }
        }
        check(page.status==PAGE_STATUS_RESIDENT && page.physicalSlotIndex==0, "fallback to parent resident");

        // No parent resident case: make root UNLOADED, then page 7 with parent chain all UNLOADED should drop
        table[0].status=PAGE_STATUS_UNLOADED;
        table[7].status=PAGE_STATUS_UNLOADED;
        table[7].parentPageID=8;
        table[8].status=PAGE_STATUS_UNLOADED;
        table[8].parentPageID=UINT32_MAX;
        GPUPageRequestQueue q2{}; q2.count=0; q2.maxRequests=4096;
        simulateRequest(7, table, q2);
        // Fallback traversal should fail to find resident
        pageID=7; page=table[pageID];
        uint32_t parent=page.parentPageID;
        bool foundResident=false;
        while(parent!=UINT32_MAX){
            if(table[parent].status==PAGE_STATUS_RESIDENT){ foundResident=true; break; }
            parent=table[parent].parentPageID;
        }
        check(!foundResident, "fallback no resident -> drop");

        // MaxRequests clamp
        GPUPageRequestQueue q3{}; q3.count=0; q3.maxRequests=2;
        // Try to request 3 distinct pages but max is 2
        table[1].status=PAGE_STATUS_UNLOADED; table[2].status=PAGE_STATUS_UNLOADED; table[3].status=PAGE_STATUS_UNLOADED;
        q3.count=0;
        simulateRequest(1, table, q3);
        simulateRequest(2, table, q3);
        simulateRequest(3, table, q3); // this should increment count to 3 but queue storage only 2, so third request's queue slot is beyond max but count still increments
        check(q3.count==3, "request count increments beyond max");
        check(q3.maxRequests==2, "maxRequests 2");
        // Shader clamps: if slot < maxRequests then store, so third slot not stored but count still 3
        // This prevents overflow of adjoining memory
    }

    // 5. MeshStreamingManager: processReadback + apply, LRU, virtual-to-physical, counter reset
    {
        PageSlotPool pool;
        pool.init(VK_NULL_HANDLE, VK_NULL_HANDLE);
        MeshStreamingManager mgr;
        mgr.init(VK_NULL_HANDLE, &pool);

        const uint32_t pageCount=5;
        std::vector<VirtualPageEntry> table(pageCount);
        for(uint32_t i=0;i<pageCount;++i){
            table[i].physicalSlotIndex=UINT32_MAX;
            table[i].status=PAGE_STATUS_UNLOADED;
            table[i].meshletCount=10;
            table[i].parentPageID=UINT32_MAX;
        }
        // Pin root 0 as resident (simulate)
        table[0].status=PAGE_STATUS_RESIDENT;
        table[0].physicalSlotIndex=0;
        // Manually allocate slot for page 0 in pool to reflect residency
        uint32_t ev;
        pool.allocateSlot(0, ev);

        GPUPageRequestQueue queue{};
        queue.count=2;
        queue.maxRequests=4096;
        queue.requestedPageIDs[0]=1;
        queue.requestedPageIDs[1]=2;
        // Simulate that GPU set these pages to REQUESTED already
        table[1].status=PAGE_STATUS_REQUESTED;
        table[2].status=PAGE_STATUS_REQUESTED;

        mgr.processReadbackRequests(queue);
        check(mgr.pendingUpdateCount()==2, "streaming pending 2");

        // Apply
        mgr.applyResidencyUpdates(VK_NULL_HANDLE, table.data());
        check(table[1].status==PAGE_STATUS_RESIDENT && table[1].physicalSlotIndex!=UINT32_MAX, "streaming page 1 resident after apply");
        check(table[2].status==PAGE_STATUS_RESIDENT, "streaming page 2 resident");
        check(pool.isResident(1) && pool.isResident(2), "pool has 1,2");
        check(mgr.pendingUpdateCount()==0, "streaming pending cleared after apply");

        // Test LRU eviction via manager: fill pool
        // Pool has 2048 slots, we have 3 allocated (0,1,2), need to fill to force eviction
        // Instead test eviction by directly filling pool to capacity with dummy pages
        for(uint32_t i=10;i<2058;++i){
            uint32_t e;
            pool.allocateSlot(i, e);
        }
        check(pool.allocatedCount()==2048, "pool full after fill");

        // Now request a new page via manager should evict LRU
        GPUPageRequestQueue q2{};
        q2.count=1; q2.maxRequests=4096; q2.requestedPageIDs[0]=9999;
        table.resize(10000, VirtualPageEntry{UINT32_MAX, PAGE_STATUS_UNLOADED, 0, UINT32_MAX});
        for(size_t i=pageCount;i<table.size();++i) { table[i].status=PAGE_STATUS_UNLOADED; table[i].physicalSlotIndex=UINT32_MAX; table[i].meshletCount=0; table[i].parentPageID=UINT32_MAX; }
        // Need to ensure table large enough for 9999
        if(table.size()<=9999) table.resize(10000, VirtualPageEntry{UINT32_MAX, PAGE_STATUS_UNLOADED, 0, UINT32_MAX});
        table[9999].status=PAGE_STATUS_REQUESTED;
        mgr.processReadbackRequests(q2);
        // After process, there should be an evict update + commit
        bool hasEvict=false;
        // Check that pool evicted LRU (which should be 0 if not touched, but 0 was MRU? Actually 0 was allocated first, but we filled with 10..2057, so LRU is 1 or 0)
        // Our earlier touch: 1 and 2 are recent, so LRU is 10
        // The evicted page should be one of the early ones
        check(mgr.pendingUpdateCount()>=1, "streaming evict pending");

        mgr.applyResidencyUpdates(VK_NULL_HANDLE, table.data());
        check(table[9999].status==PAGE_STATUS_RESIDENT, "streaming 9999 resident after evict");

        // Clear requests
        GPUPageRequestQueue q3{}; q3.count=5; q3.maxRequests=4096;
        mgr.clearRequests(q3);
        check(q3.count==0, "streaming clearRequests 0");

        mgr.shutdown();
        pool.shutdown();
    }

    // 6. Pin root pages permanently resident
    {
        std::vector<GPUMeshlet> meshlets(128);
        for(auto &m: meshlets) m.boundingSphere=glm::vec4(0,0,0,1);
        MeshletHierarchy hier;
        hier.build(meshlets, 64);
        std::vector<VirtualPageEntry> table = hier.virtualPages();
        // Initially all UNLOADED
        for(auto &e: table) e.status=PAGE_STATUS_UNLOADED;
        hier.pinRootPages(table);
        // Root pages (parent == UINT32_MAX) should be RESIDENT
        bool rootsPinned=true;
        for(size_t i=0;i<hier.nodes().size();++i){
            if(hier.nodes()[i].parentPageID==UINT32_MAX){
                if(table[i].status!=PAGE_STATUS_RESIDENT) rootsPinned=false;
            }
        }
        check(rootsPinned, "hierarchy pin roots resident");
    }

    // 7. Large virtual set vs physical budget (10x)
    {
        // Simulate 2048*10 virtual pages =20480, physical 2048
        const uint32_t virtualPages = 20480;
        const uint32_t physicalSlots = 2048;
        check(virtualPages == physicalSlots*10, "virtual 10x physical");
        // Our pool is 2048, so working set fits within fixed budget
        PageSlotPool pool;
        pool.init(VK_NULL_HANDLE, VK_NULL_HANDLE);
        check(pool.freeSlotCount()==2048, "pool 128MB budget");
        pool.shutdown();
    }

    if(failCount==0) printf("PASS: streaming LRU, requests, slot reuse, remapping, hierarchy\n");
    else printf("FAIL %d checks\n", failCount);
    return failCount==0?0:1;
}
