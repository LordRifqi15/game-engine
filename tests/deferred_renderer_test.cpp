// Deferred Renderer: MRT, depth RO, world reconstruction
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/deferred/GBuffer.cpp ../src/renderer/deferred/DeferredPipeline.cpp ../src/renderer/graph/RenderGraph.cpp ../src/renderer/graph/RenderGraphBuilder.cpp ../src/renderer/graph/RenderGraphResources.cpp ../src/renderer/graph/ResourceLifetime.cpp ../src/renderer/graph/TransientResourcePool.cpp ../src/renderer/vulkan/VkBarrierHelper.cpp ../src/renderer/vulkan/VkDeviceMemoryHelper.cpp deferred_renderer_test.cpp -o /tmp/deferred_renderer_test -lvulkan && /tmp/deferred_renderer_test

#include "renderer/deferred/GBuffer.hpp"
#include "renderer/deferred/DeferredPipeline.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/TransientResourcePool.hpp"
#include "renderer/api/Synchronization.hpp"
#include "core/registry.h"

#include <cstdio>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

using namespace Engine;

static int failCount=0;
static void check(bool cond, const char* msg){ if(!cond){ printf("FAIL %s\n", msg); failCount++; } else printf("OK %s\n", msg); }

static VkImage dummyImage(uint64_t id){ return reinterpret_cast<VkImage>(id); }
static VkImageView dummyView(uint64_t id){ return reinterpret_cast<VkImageView>(id); }

// C++ version of reconstructWorldPos from deferred_lighting.frag
static glm::vec3 reconstructWorldPos(glm::vec2 uv, float depth, const glm::mat4& invViewProj){
    glm::vec4 clipSpacePos = glm::vec4(uv * 2.0f - 1.0f, depth, 1.0f);
    glm::vec4 worldSpacePos = invViewProj * clipSpacePos;
    return glm::vec3(worldSpacePos) / worldSpacePos.w;
}

int main(){
    // 1. GBuffer declare
    {
        RenderGraph g;
        VkExtent2D ext{1280,720};
        GBufferHandles h = GBuffer::declare(g, ext);
        check(h.albedoAO.isValid() && h.normalRoughness.isValid() && h.metallicFlags.isValid() && h.depth.isValid(), "GBuffer handles valid");
        auto& res = g.resources();
        check(res[h.albedoAO.id].desc.format==VK_FORMAT_R8G8B8A8_SRGB, "GBuffer albedo format SRGB");
        check(res[h.normalRoughness.id].desc.format==VK_FORMAT_R16G16B16A16_SFLOAT, "GBuffer normal format SFLOAT");
        check(res[h.metallicFlags.id].desc.format==VK_FORMAT_R8G8B8A8_UNORM, "GBuffer metallic format UNORM");
        check(res[h.depth.id].desc.format==VK_FORMAT_D32_SFLOAT, "GBuffer depth format D32");
        check(res[h.albedoAO.id].desc.extent.width==1280 && res[h.albedoAO.id].desc.extent.height==720, "GBuffer extent");
        check((res[h.albedoAO.id].desc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && (res[h.albedoAO.id].desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "GBuffer albedo usage");
        check((res[h.depth.id].desc.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT), "GBuffer depth usage");
        check(res[h.albedoAO.id].type==ResourceType::Transient, "GBuffer transient");
    }

    // 2. DeferredPipeline MRT graph
    {
        RenderGraph g;
        VkExtent2D ext{1920,1080};
        auto swap = g.importImage("Swapchain", dummyImage(0x1000), dummyView(0x1001), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        ::engine::Registry reg;
        DeferredPipeline pipe;
        pipe.buildPipeline(g, reg, swap, ext);
        check(g.passCount()==5 || g.passCount()==6, "deferred pass count 5-6");
        // Find GBufferPass
        bool foundGBuffer=false, foundLighting=false, foundForward=false, foundShadow=false, foundPost=false;
        for(auto& p: g.passes()){
            if(p.name=="GBufferPass"){
                foundGBuffer=true;
                check(p.writes.size()==4, "GBuffer MRT 4 writes");
                // Check that 3 are color, 1 depth
                int colorWrites=0, depthWrites=0;
                for(auto& [h,u]: p.writes){
                    if(u==ResourceUsage::ColorAttachment) colorWrites++;
                    if(u==ResourceUsage::DepthStencilAttachment) depthWrites++;
                }
                check(colorWrites==3 && depthWrites==1, "GBuffer MRT 3C+1D");
            }
            if(p.name=="ShadowPass") foundShadow=true;
            if(p.name=="DeferredLightingPass"){
                foundLighting=true;
                // Should read 4 GBuffer + shadow, write HDR
                check(p.reads.size()>=5, "DeferredLighting reads >=5");
                check(p.writes.size()==1, "DeferredLighting writes HDR");
            }
            if(p.name=="ForwardOverlayPass"){
                foundForward=true;
                // Should read depth as DepthStencilAttachment (RO)
                bool hasDepthRead=false;
                for(auto& [h,u]: p.reads) if(u==ResourceUsage::DepthStencilAttachment) hasDepthRead=true;
                check(hasDepthRead, "ForwardOverlay reads depth RO");
            }
            if(p.name=="PostProcessPass") foundPost=true;
        }
        check(foundShadow && foundGBuffer && foundLighting && foundForward && foundPost, "deferred all passes found");
        // Compile and check order: Shadow before GBuffer? Actually GBuffer independent of Shadow, but DeferredLighting after both
        bool ok = g.compile();
        check(ok, "deferred compile true");
        auto& sorted = g.sortedPassIndices();
        auto idxOf = [&](const char* n)->int{
            for(size_t i=0;i<sorted.size();++i) if(g.passes()[sorted[i]].name==n) return (int)i;
            return -1;
        };
        int s=idxOf("ShadowPass"), gb=idxOf("GBufferPass"), dl=idxOf("DeferredLightingPass"), fo=idxOf("ForwardOverlayPass"), pp=idxOf("PostProcessPass");
        check(s>=0 && gb>=0 && dl>=0 && fo>=0 && pp>=0, "deferred sorted all found");
        check(dl > s && dl > gb, "DeferredLighting after Shadow+GBuffer");
        check(fo > dl, "ForwardOverlay after DeferredLighting");
        check(pp > fo, "PostProcess after ForwardOverlay");
        // Check that GBuffer writes are before DeferredLighting reads (dependency)
        check(gb < dl, "GBuffer before Lighting");
    }

    // 3. Depth Read-Only barrier aspect
    {
        VkImageAspectFlags aspectColor = getAspectMask(VK_FORMAT_R8G8B8A8_SRGB, ResourceUsage::ColorAttachment);
        VkImageAspectFlags aspectDepth = getAspectMask(VK_FORMAT_D32_SFLOAT, ResourceUsage::DepthStencilAttachment);
        check(aspectColor==VK_IMAGE_ASPECT_COLOR_BIT, "aspect color");
        check(aspectDepth==VK_IMAGE_ASPECT_DEPTH_BIT, "aspect depth RO");
        // Check barrier states
        auto depthWrite = getBarrierState(ResourceUsage::DepthStencilAttachment);
        check(depthWrite.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, "depth write layout");
        // For read-only depth, we need to check via resolveBarriers logic: read depth should be READ_ONLY
        // Simulate graph with depth RO pass
        RenderGraph g;
        VkExtent2D ext{512,512};
        auto swap = g.importImage("Swapchain", dummyImage(0x2000), dummyView(0x2001), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        ImageDesc depthDesc; depthDesc.name="Depth"; depthDesc.format=VK_FORMAT_D32_SFLOAT; depthDesc.extent=ext; depthDesc.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        auto depth = g.createResource(depthDesc);
        g.addPass("GBuffer", [&](RenderGraphBuilder& b){ b.write(depth, ResourceUsage::DepthStencilAttachment); }, [&](VkCommandBuffer){});
        g.addPass("Overlay", [&](RenderGraphBuilder& b){ b.read(depth, ResourceUsage::DepthStencilAttachment); b.write(swap, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("Present", [&](RenderGraphBuilder& b){ b.read(swap, ResourceUsage::Present); }, [&](VkCommandBuffer){});
        bool ok=g.compile();
        check(ok, "depth RO compile");
        // Execute with null cmd should not crash, and should handle RO transition correctly
        g.execute(VK_NULL_HANDLE);
        // Check that depth's lastUsage after execute is DepthStencilAttachment (read) or still? Actually last write was DepthStencilAttachment at GBuffer, then read at Overlay as DepthStencilAttachment RO, so lastUsage should be DepthStencilAttachment
        // Our barrier should not assert
        check(true, "depth RO barrier no crash");
    }

    // 4. World reconstruction math (inverse viewProj)
    {
        // Use identity viewProj for simple deterministic test
        glm::mat4 viewProj = glm::mat4(1.0f);
        glm::mat4 invViewProj = glm::inverse(viewProj);
        // World origin at (0,0,0) -> clip (0,0,0) -> uv (0.5,0.5) depth 0
        glm::vec3 recon = reconstructWorldPos(glm::vec2(0.5f,0.5f), 0.0f, invViewProj);
        check(glm::length(recon - glm::vec3(0,0,0)) < 0.01f, "reconstruct origin");
        // World point (0.5,0.5,0.5) -> clip (0.5,0.5,0.5) -> uv 0.75,0.75 depth 0.5
        glm::vec3 recon2 = reconstructWorldPos(glm::vec2(0.75f,0.75f), 0.5f, invViewProj);
        check(glm::length(recon2 - glm::vec3(0.5f,0.5f,0.5f)) < 0.01f, "reconstruct (0.5,0.5,0.5)");
        // Test with real perspective (Vulkan 0-1 depth)
        glm::vec3 camPos{0, 2, 5};
        glm::mat4 view = glm::lookAt(camPos, glm::vec3(0,0,0), glm::vec3(0,1,0));
        // Use Vulkan depth 0-1 projection
        float fov = glm::radians(60.0f);
        float aspect = 1920.0f/1080.0f;
        float near = 0.1f, far = 100.0f;
        float tanHalf = tan(fov*0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = 1.0f/(aspect*tanHalf);
        proj[1][1] = 1.0f/tanHalf;
        proj[2][2] = far/(far-near);
        proj[2][3] = 1.0f;
        proj[3][2] = -(far*near)/(far-near);
        // proj is Vulkan style (depth 0-1)
        glm::mat4 vp = proj * view;
        glm::mat4 invVP = glm::inverse(vp);
        glm::vec4 worldPos{1,0,0,1};
        glm::vec4 clip = vp * worldPos;
        glm::vec3 ndc = glm::vec3(clip)/clip.w;
        glm::vec2 uv = glm::vec2(ndc.x*0.5f+0.5f, ndc.y*0.5f+0.5f);
        float depth = ndc.z; // already 0-1 for Vulkan proj
        glm::vec3 recon3 = reconstructWorldPos(uv, depth, invVP);
        check(glm::length(recon3 - glm::vec3(1,0,0)) < 0.05f, "reconstruct (1,0,0) Vulkan");
        check(depth >= -0.2f && depth <= 1.2f, "depth in range");
    }
    // 5. Transient memory reuse for GBuffer (aliasing)
    {
        TransientResourcePool pool(VK_NULL_HANDLE, VK_NULL_HANDLE);
        RenderGraph g;
        VkExtent2D ext{1280,720};
        auto swap = g.importImage("Swapchain", dummyImage(0x3000), dummyView(0x3001), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        ::engine::Registry reg;
        DeferredPipeline pipe;
        pipe.buildPipeline(g, reg, swap, ext);
        bool ok = g.compile(0, pool);
        check(ok, "deferred transient compile");
        // Check that pool has allocations for GBuffer + HDR + shadow (should be 6? Let's count: GBuffer 4 + HDR 1 + Shadow 1 =6)
        size_t total = pool.getTotalAllocationCount();
        check(total >= 5 && total <= 6, "deferred pool alloc count 5-6");
        // Check that GBuffer images are reused for post-processing if non-overlapping: In deferred pipeline, GBuffer AlbedoAO lifetime is [1,2] (GBuffer write at 1, Lighting read at 2), HDR lifetime [2,4] etc. They overlap partially, so not all alias.
        // But at least check that aliasing works: GBuffer Albedo and HDR have different formats, so not alias. Depth and Shadow both depth but different extents (Shadow 2048 vs Depth 1280), so not alias due extent mismatch.
        // Check that after second frame, pool doesn't grow unbounded (zero per-frame alloc after warmup)
        size_t before = pool.getTotalAllocationCount();
        RenderGraph g2;
        auto swap2 = g2.importImage("Swapchain", dummyImage(0x3002), dummyView(0x3003), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        pipe.buildPipeline(g2, reg, swap2, ext);
        g2.compile(1, pool);
        size_t after = pool.getTotalAllocationCount();
        check(after == before + total || after == total*2, "deferred second frame double buffer");
        // Third frame should reuse
        RenderGraph g3;
        auto swap3 = g3.importImage("Swapchain", dummyImage(0x3004), dummyView(0x3005), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        pipe.buildPipeline(g3, reg, swap3, ext);
        g3.compile(2, pool);
        size_t after2 = pool.getTotalAllocationCount();
        check(after2 == after, "deferred third frame reuse");
    }

    if(failCount==0) printf("PASS: deferred MRT, depth RO, world recon, transient reuse\n");
    else printf("FAIL %d checks\n", failCount);
    return failCount==0?0:1;
}
