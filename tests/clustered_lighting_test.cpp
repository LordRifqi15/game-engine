// Clustered Lighting: CPU math, AABB, buffer DAG, deferred integration
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/lighting/ClusteredLighting.cpp ../src/renderer/deferred/GBuffer.cpp ../src/renderer/deferred/DeferredPipeline.cpp ../src/renderer/graph/RenderGraph.cpp ../src/renderer/graph/RenderGraphBuilder.cpp ../src/renderer/graph/RenderGraphResources.cpp ../src/renderer/graph/ResourceLifetime.cpp ../src/renderer/graph/TransientResourcePool.cpp ../src/renderer/vulkan/VkBarrierHelper.cpp ../src/renderer/vulkan/VkDeviceMemoryHelper.cpp clustered_lighting_test.cpp -o /tmp/clustered_lighting_test -lvulkan && /tmp/clustered_lighting_test

#include <vulkan/vulkan.h>
#include "renderer/api/Synchronization.hpp"
#include "renderer/lighting/ClusteredLighting.hpp"
#include "renderer/lighting/LightTypes.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include "renderer/deferred/DeferredPipeline.hpp"
#include "renderer/deferred/GBuffer.hpp"
#include "core/registry.h"

#include <cstdio>
#include <vector>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

using namespace Engine;

static int failCount=0;
static void check(bool cond, const char* msg){ if(!cond){ printf("FAIL %s\n", msg); failCount++; } else printf("OK %s\n", msg); }

static VkImage dummyImage(uint64_t id){ return reinterpret_cast<VkImage>(id); }
static VkImageView dummyView(uint64_t id){ return reinterpret_cast<VkImageView>(id); }
static VkBuffer dummyBuffer(uint64_t id){ return reinterpret_cast<VkBuffer>(id); }

int main(){
    // 1. Grid math
    {
        VkExtent2D ext{1280,720};
        uint32_t gx = ClusteredLighting::computeGridX(ext);
        uint32_t gy = ClusteredLighting::computeGridY(ext);
        check(gx == (1280 + 64 -1)/64, "gridX 1280");
        check(gy == (720 + 64 -1)/64, "gridY 720");
        check(gx==20 && gy==12, "grid 20x12 for 1280x720/64");
        VkExtent2D ext2{1920,1080};
        check(ClusteredLighting::computeGridX(ext2)==30, "gridX 1920");
        check(ClusteredLighting::computeGridY(ext2)==17, "gridY 1080 (ceil 16.875)");
        // Total clusters
        ClusteredLighting cl;
        cl.init(VK_NULL_HANDLE, VK_NULL_HANDLE);
        std::vector<GPULight> empty;
        glm::mat4 view(1.0f), proj(1.0f), invProj(1.0f);
        cl.updateLightBuffers(empty, view, proj, invProj, ext, 0.1f, 100.0f);
        check(cl.totalClusters() == gx*gy*ClusteredLighting::CLUSTER_SLICES_Z, "totalClusters math");
        check(cl.totalClusters() == 20*12*24, "total 5760");
        check(cl.gridX()==20 && cl.gridY()==12 && cl.gridZ()==24, "grid dims");
    }

    // 2. Slice index (log depth)
    {
        float zNear=0.1f, zFar=100.0f;
        uint32_t slices=24;
        // near -> 0
        check(sliceIndex(zNear, zNear, zFar, slices)==0, "slice near 0");
        // far -> slices-1
        check(sliceIndex(zFar, zNear, zFar, slices)==slices-1, "slice far last");
        // mid log
        uint32_t mid = sliceIndex(1.0f, zNear, zFar, slices);
        check(mid > 0 && mid < slices-1, "slice mid interior");
        // monotonic
        float prev = zNear;
        uint32_t prevIdx=0;
        bool mono=true;
        for(float z=zNear; z<zFar; z*=2.0f){
            uint32_t idx=sliceIndex(z, zNear, zFar, slices);
            if(idx < prevIdx) mono=false;
            prevIdx=idx;
        }
        check(mono, "slice monotonic");
    }

    // 3. Cluster AABB
    {
        ClusteredLighting cl;
        cl.init(VK_NULL_HANDLE, VK_NULL_HANDLE);
        std::vector<GPULight> empty;
        VkExtent2D ext{1280,720};
        glm::mat4 view(1.0f);
        // perspective proj for proper scale
        float aspect=1280.0f/720.0f;
        float fov=glm::radians(60.0f);
        float tanHalf=tan(fov*0.5f);
        float zNear=0.1f, zFar=100.0f;
        glm::mat4 proj(0.0f);
        proj[0][0]=1.0f/(aspect*tanHalf);
        proj[1][1]=1.0f/tanHalf;
        proj[2][2]=zFar/(zFar - zNear);
        proj[2][3]=1.0f;
        proj[3][2]=-(zFar*zNear)/(zFar - zNear);
        glm::mat4 invProj=glm::inverse(proj);
        cl.updateLightBuffers(empty, view, proj, invProj, ext, zNear, zFar);
        auto uniforms = cl.getUniforms();
        // AABB for cluster 0 should be valid and min<max
        glm::vec3 minA, maxA;
        ClusteredLighting::computeClusterAABB(0, uniforms, minA, maxA);
        check(minA.x < maxA.x && minA.y < maxA.y && minA.z < maxA.z, "AABB 0 min<max");
        // Last cluster
        uint32_t total=cl.totalClusters();
        ClusteredLighting::computeClusterAABB(total-1, uniforms, minA, maxA);
        check(minA.x < maxA.x && minA.y < maxA.y, "AABB last min<max");
        // Out of bounds should give degenerate
        ClusteredLighting::computeClusterAABB(total+10, uniforms, minA, maxA);
        check(minA==glm::vec3(0) && maxA==glm::vec3(0), "AABB out of bounds degenerate");
        // All clusters should cover screen without gaps: tile X extents should tile
        // Check neighbor AABBs share boundary in X
        glm::vec3 min0, max0, min1, max1;
        ClusteredLighting::computeClusterAABB(0, uniforms, min0, max0);
        ClusteredLighting::computeClusterAABB(1, uniforms, min1, max1);
        // tile 0 and 1 are adjacent in X (same Y,Z)
        // Their X ranges should be adjacent (max0.x ~ min1.x) within epsilon
        check(fabs(max0.x - min1.x) < 5.0f, "AABB neighbor X adjacency");
    }

    // 4. Sphere-AABB
    {
        glm::vec3 minA{-1,-1,-1}, maxA{1,1,1};
        check(testSphereAABB(glm::vec3(0,0,0), 0.5f, minA, maxA), "sphere inside");
        check(testSphereAABB(glm::vec3(2,0,0), 0.5f, minA, maxA)==false, "sphere outside");
        check(testSphereAABB(glm::vec3(1,0,0), 0.5f, minA, maxA), "sphere touching");
        check(testSphereAABB(glm::vec3(0,0,0), 10.0f, minA, maxA), "sphere large covers");
    }

    // 5. Cluster index from UV + viewZ
    {
        ClusteredLighting cl;
        cl.init(VK_NULL_HANDLE, VK_NULL_HANDLE);
        VkExtent2D ext{1280,720};
        glm::mat4 view(1.0f), proj(1.0f), invProj(1.0f);
        cl.updateLightBuffers({}, view, proj, invProj, ext, 0.1f, 100.0f);
        auto uniforms = cl.getUniforms();
        // Center of screen, near depth -> tile (10,6) slice 0
        glm::vec2 uv{0.5f, 0.5f};
        uint32_t idx = getClusterIndex(uv, -0.2f, uniforms);
        check(idx < cl.totalClusters(), "cluster index in range");
        // corners
        uint32_t idx00 = getClusterIndex(glm::vec2(0.0f,0.0f), -0.2f, uniforms);
        uint32_t idx11 = getClusterIndex(glm::vec2(0.99f,0.99f), -0.2f, uniforms);
        check(idx00 != idx11, "cluster index varies with UV");
        // depth variation
        uint32_t idxNear = getClusterIndex(uv, -0.15f, uniforms);
        uint32_t idxFar = getClusterIndex(uv, -50.0f, uniforms);
        check(idxNear != idxFar, "cluster index varies with depth");
    }

    // 6. RenderGraph buffer DAG: compute -> fragment
    {
        RenderGraph g;
        VkExtent2D ext{512,512};
        auto swap = g.importImage("Swapchain", dummyImage(0x600), dummyView(0x601), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        // Create buffers
        BufferDesc lightDesc; lightDesc.name="Lights"; lightDesc.size=4096*sizeof(GPULight); lightDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        BufferDesc gridDesc; gridDesc.name="Grid"; gridDesc.size=20*12*24*sizeof(ClusterCell); gridDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        BufferDesc idxDesc; idxDesc.name="Indices"; idxDesc.size=4+ 20*12*24*128*4; idxDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto lightBuf = g.createBuffer(lightDesc);
        auto gridBuf = g.createBuffer(gridDesc);
        auto idxBuf = g.createBuffer(idxDesc);
        // Also need an image for fragment pass to write
        ImageDesc hdr; hdr.name="HDR"; hdr.format=VK_FORMAT_R16G16B16A16_SFLOAT; hdr.extent=ext; hdr.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        auto hdrTarget = g.createResource(hdr);

        std::vector<std::string> order;
        g.addPass("CullCompute",
            [&](RenderGraphBuilder& b){
                b.read(lightBuf, BufferUsage::ComputeRead);
                b.write(gridBuf, BufferUsage::ComputeWrite);
                b.write(idxBuf, BufferUsage::ComputeWrite);
            },
            [&](VkCommandBuffer){ order.push_back("CullCompute"); }
        );
        g.addPass("Lighting",
            [&](RenderGraphBuilder& b){
                b.read(gridBuf, BufferUsage::FragmentRead);
                b.read(idxBuf, BufferUsage::FragmentRead);
                b.read(lightBuf, BufferUsage::FragmentRead);
                b.write(hdrTarget, ResourceUsage::ColorAttachment);
            },
            [&](VkCommandBuffer){ order.push_back("Lighting"); }
        );
        g.addPass("Present",
            [&](RenderGraphBuilder& b){ b.read(hdrTarget, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){ order.push_back("Present"); }
        );
        // Also add a dead compute pass that writes unused buffer -> should be culled
        BufferDesc deadDesc; deadDesc.name="DeadBuf"; deadDesc.size=1024; deadDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto deadBuf = g.createBuffer(deadDesc);
        g.addPass("DeadCull",
            [&](RenderGraphBuilder& b){ b.write(deadBuf, BufferUsage::ComputeWrite); },
            [&](VkCommandBuffer){ order.push_back("DeadCull"); }
        );
        check(g.passCount()==4, "buffer DAG 4 passes declared");
        bool ok=g.compile();
        check(ok, "buffer DAG compile true");
        // Check pruning: DeadCull should be culled (writes deadBuf not read)
        bool deadFound=false;
        for(auto idx:g.sortedPassIndices()) if(g.passes()[idx].name=="DeadCull") deadFound=true;
        check(!deadFound, "buffer dead pass culled");
        // Check order: Cull before Lighting before Present
        auto& sorted=g.sortedPassIndices();
        auto idxOf=[&](const char* n)->int{ for(size_t i=0;i<sorted.size();++i) if(g.passes()[sorted[i]].name==n) return (int)i; return -1; };
        int cull=idxOf("CullCompute"), light=idxOf("Lighting"), pres=idxOf("Present");
        check(cull>=0 && light>=0 && pres>=0, "buffer DAG all live found");
        check(cull < light && light < pres, "buffer DAG order Compute->Fragment->Present");
        // Execute with barriers (VK_NULL_HANDLE should not crash)
        order.clear();
        g.execute(VK_NULL_HANDLE);
        check(order.size()==3 && order[0]=="CullCompute" && order[1]=="Lighting" && order[2]=="Present", "buffer DAG execute order");
        // Barrier state checks for buffers
        Engine::BarrierState csRead = Engine::getBarrierState(BufferUsage::ComputeRead);
        check(csRead.stageMask==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && csRead.accessMask==VK_ACCESS_SHADER_READ_BIT, "barrier ComputeRead");
        Engine::BarrierState csWrite = Engine::getBarrierState(BufferUsage::ComputeWrite);
        check(csWrite.accessMask==VK_ACCESS_SHADER_WRITE_BIT, "barrier ComputeWrite");
        Engine::BarrierState fragRead = Engine::getBarrierState(BufferUsage::FragmentRead);
        check(fragRead.stageMask==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, "barrier FragmentRead");
        // Clear should wipe buffers and passes
        size_t beforeBuf = g.bufferCount();
        check(beforeBuf==4, "buffer count 4");
        g.clear();
        check(g.passCount()==0 && g.bufferCount()==0 && g.resourceCount()==0, "buffer clear wipes");
    }

    // 7. DeferredPipeline clustered integration
    {
        RenderGraph g;
        VkExtent2D ext{1280,720};
        auto swap = g.importImage("Swapchain", dummyImage(0x700), dummyView(0x701), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        ::engine::Registry reg;
        DeferredPipeline pipe;
        pipe.buildPipeline(g, reg, swap, ext);
        check(g.passCount()==6, "deferred clustered pass count 6");
        // Find ClusterCull
        bool foundCull=false, foundLight=false;
        RenderPassNode* cullNode=nullptr;
        RenderPassNode* lightNode=nullptr;
        for(auto& p: g.passes()){
            if(p.name=="ClusterCull"){ foundCull=true; cullNode=(RenderPassNode*)&p; }
            if(p.name=="DeferredLightingPass"){ foundLight=true; lightNode=(RenderPassNode*)&p; }
        }
        check(foundCull && foundLight, "deferred found ClusterCull + Lighting");
        // Cull should write 2 buffers, read 1
        if(cullNode){
            check(cullNode->bufferWrites.size()==2, "ClusterCull 2 buffer writes");
            check(cullNode->bufferReads.size()==1, "ClusterCull 1 buffer read");
            bool hasComputeWrite=false, hasComputeRead=false;
            for(auto& [h,u]: cullNode->bufferWrites) if(u==BufferUsage::ComputeWrite) hasComputeWrite=true;
            for(auto& [h,u]: cullNode->bufferReads) if(u==BufferUsage::ComputeRead) hasComputeRead=true;
            check(hasComputeWrite && hasComputeRead, "ClusterCull buffer usages Compute");
        }
        if(lightNode){
            // Lighting should now read 3 buffers (lights, grid, indices) plus 5 images
            check(lightNode->bufferReads.size()>=3, "DeferredLighting 3 buffer reads");
            bool hasFragRead=false;
            for(auto& [h,u]: lightNode->bufferReads) if(u==BufferUsage::FragmentRead) hasFragRead=true;
            check(hasFragRead, "DeferredLighting buffer FragmentRead");
        }
        bool ok=g.compile();
        check(ok, "deferred clustered compile true");
        auto& sorted=g.sortedPassIndices();
        auto idxOf=[&](const char* n)->int{ for(size_t i=0;i<sorted.size();++i) if(g.passes()[sorted[i]].name==n) return (int)i; return -1; };
        int cullIdx=idxOf("ClusterCull"), lightIdx=idxOf("DeferredLightingPass");
        int gbufIdx=idxOf("GBufferPass"), postIdx=idxOf("PostProcessPass");
        check(cullIdx>=0 && lightIdx>=0 && gbufIdx>=0 && postIdx>=0, "deferred clustered sorted found");
        check(cullIdx < lightIdx, "ClusterCull before Lighting");
        check(gbufIdx < lightIdx, "GBuffer before Lighting");
        check(lightIdx < postIdx, "Lighting before Post");
        // Verify total buffer resources = 3
        check(g.bufferCount()==3, "deferred buffer count 3");
        // Execute shouldn't crash
        g.execute(VK_NULL_HANDLE);
        check(true, "deferred clustered execute no crash");
    }

    // 8. Shader existence: cluster_cull.comp should have been compiled via build (shaders target)
    // We check that deferred_lighting.frag contains clustered keywords (already verified by build)
    // Here just sanity: ensure ClusterCell size matches shader expectation (8 bytes)
    {
        check(sizeof(ClusterCell)==8, "ClusterCell 8 bytes");
        check(sizeof(GPULight)==64, "GPULight 64 bytes");
        // MAX_LIGHTS * MAX cluster indices fit in buffer sizes we declared
        size_t total = 20*12*24;
        size_t clusterBytes = total*sizeof(ClusterCell);
        size_t indexBytes = 4 + total*128*4;
        check(clusterBytes == 5760*8, "cluster buffer bytes");
        check(indexBytes == 4 + 5760*512, "index buffer bytes");
    }

    if(failCount==0) printf("PASS: clustered CPU, AABB, buffer DAG, deferred integration\n");
    else printf("FAIL %d checks\n", failCount);
    return failCount==0?0:1;
}
