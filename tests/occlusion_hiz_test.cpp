// Occlusion & HiZ: AABB projection, mip selection, frustum culling
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/culling/HiZPyramid.cpp ../src/renderer/culling/GPUCullingSystem.cpp ../src/renderer/graph/RenderGraph.cpp ../src/renderer/graph/RenderGraphBuilder.cpp ../src/renderer/graph/RenderGraphResources.cpp ../src/renderer/graph/ResourceLifetime.cpp ../src/renderer/graph/TransientResourcePool.cpp ../src/renderer/vulkan/VkBarrierHelper.cpp ../src/renderer/vulkan/VkDeviceMemoryHelper.cpp occlusion_hiz_test.cpp -o /tmp/occlusion_hiz_test -lvulkan && /tmp/occlusion_hiz_test

#include "renderer/culling/HiZPyramid.hpp"
#include "renderer/culling/CullingTypes.hpp"
#include "renderer/culling/GPUCullingSystem.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include "renderer/graph/TransientResourcePool.hpp"
#include "renderer/api/Synchronization.hpp"
#include "ecs/components/BoundsComponent.hpp"

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
    // 1. HiZ mip levels & extents
    {
        VkExtent2D ext{1920,1080};
        uint32_t mips = HiZPyramid::computeMipLevels(ext);
        // max(1920,1080)=1920, log2=10.9 floor 10 +1 =11
        check(mips==11, "HiZ mips 1920x1080 =11");
        check(HiZPyramid::computeMipLevels({1024,1024})==11, "HiZ mips 1024=11 (2^10+1)");
        check(HiZPyramid::computeMipLevels({512,512})==10, "HiZ mips 512=10");
        check(HiZPyramid::computeMipLevels({1,1})==1, "HiZ mips 1x1=1");
        check(HiZPyramid::computeMipLevels({0,0})==1, "HiZ mips 0=1");

        VkExtent2D base = HiZPyramid::pyramidBaseExtent(ext);
        check(base.width==960 && base.height==540, "HiZ pyramid base half");
        check(HiZPyramid::pyramidBaseExtent({1,1}).width==1, "HiZ base clamp 1");

        VkExtent2D mip0 = HiZPyramid::mipExtent(base, 0);
        check(mip0.width==960 && mip0.height==540, "HiZ mip0 base");
        VkExtent2D mip1 = HiZPyramid::mipExtent(base, 1);
        check(mip1.width==480 && mip1.height==270, "HiZ mip1 half");
        VkExtent2D mipLast = HiZPyramid::mipExtent(base, 10);
        check(mipLast.width==1 && mipLast.height==1, "HiZ last mip 1x1");

        // Verify chain reaches 1x1 at max mip
        uint32_t maxMip = mips-1;
        VkExtent2D last = HiZPyramid::mipExtent(ext, maxMip);
        check(last.width==1 && last.height==1, "HiZ full chain 1x1");
    }

    // 2. Max-depth downsample (conservative)
    {
        check(HiZPyramid::downsampleMaxDepth(0.1f,0.5f,0.3f,0.2f)==0.5f, "downsample max 0.5");
        check(HiZPyramid::downsampleMaxDepth(1.0f,1.0f,1.0f,1.0f)==1.0f, "downsample max 1");
        check(HiZPyramid::downsampleMaxDepth(0.0f,0.0f,0.0f,0.9f)==0.9f, "downsample max 0.9");
        // Vulkan depth: far=1, near=0, max preserves farthest (occluder)
        check(HiZPyramid::downsampleMaxDepth(0.2f,0.8f,0.4f,0.6f) > 0.7f, "downsample conservative");
    }

    // 3. CullingTypes layout (std430, Vulkan indirect match)
    {
        check(sizeof(GPUInstanceData)== 64+16+16, "GPUInstanceData size"); // mat4 64 + vec4 16 + 4*4=16 =96
        check(sizeof(GPUInstanceData)==96, "GPUInstanceData 96");
        check(alignof(GPUInstanceData)==16, "GPUInstanceData align 16");
        check(sizeof(GPUIndirectCommand)>=20, "GPUIndirectCommand >=20 (5*4)");
        // But spec says alignas(16), so size may be 32 due to padding? Check spec says alignas(16) but fields are 5*4=20, with align 16 the size should be 32? Let's check actual.
        // Our struct is alignas(16) but contains 20 bytes, padded to 32? Actually alignas doesn't pad size to 16 unless needed; but spec says alignas(16) for SSBO.
        // We check that VkDrawIndexedIndirectCommand matches: it is 20 bytes as well.
        check(sizeof(VkDrawIndexedIndirectCommand)==20, "VkDraw size 20");
        check(sizeof(GPUIndirectCommand) >= sizeof(VkDrawIndexedIndirectCommand), "GPUIndirect >= VkDraw");
        check(sizeof(GPUIndirectCommand)==32, "GPUIndirect 32 due to alignas(16)");
        // CullingUniforms
        check(sizeof(CullingUniforms) >= 64+64+ 6*16 + 8+4+4+4+4, "CullingUniforms min size");
        check(alignof(CullingUniforms)==16, "CullingUniforms align 16");
    }

    // 4. BoundsComponent
    {
        BoundsComponent b = BoundsComponent::fromSphere(glm::vec3(1,2,3), 2.0f);
        check(b.center==glm::vec3(1,2,3) && b.radius==2.0f, "Bounds fromSphere center/radius");
        check(b.aabbMin==glm::vec3(-1,0,1) && b.aabbMax==glm::vec3(3,4,5), "Bounds fromSphere AABB");
        BoundsComponent b2 = BoundsComponent::fromAABB(glm::vec3(0,0,0), glm::vec3(2,2,2));
        check(b2.center==glm::vec3(1,1,1), "Bounds fromAABB center");
        check(fabs(b2.radius - glm::length(glm::vec3(1,1,1))) < 0.001f, "Bounds fromAABB radius");
        // Also test engine alias
        engine::BoundsComponent eb;
        check(eb.radius==1.0f, "engine alias Bounds");
    }

    // 5. Frustum culling (sphere vs 6 planes)
    {
        // Build a perspective viewProj (Vulkan RH_ZO)
        glm::vec3 camPos{0,0,5};
        glm::mat4 view = glm::lookAt(camPos, glm::vec3(0,0,0), glm::vec3(0,1,0));
        float fov=glm::radians(60.0f), aspect=16.0f/9.0f, zn=0.1f, zf=100.0f;
        glm::mat4 proj = glm::perspectiveRH_ZO(fov, aspect, zn, zf);
        // glm perspective has Y inverted for Vulkan vs OpenGL; flip Y to match Vulkan NDC
        // Actually glm::perspectiveRH_ZO already does Vulkan 0-1, but Y is still OpenGL style; Vulkan clip Y is inverted.
        // For frustum test, we don't care about Y, just keep as is.
        glm::mat4 vp = proj * view;
        glm::vec4 planes[6];
        HiZPyramid::extractFrustumPlanes(vp, planes);
        // Sphere at origin radius 0.5 should be visible
        check(HiZPyramid::isSphereFrustumVisible(glm::vec3(0,0,0), 0.5f, planes), "frustum origin visible");
        // Far away sphere behind camera
        check(!HiZPyramid::isSphereFrustumVisible(glm::vec3(0,0,20), 1.0f, planes), "frustum behind cam culled");
        // Sphere far to side
        check(!HiZPyramid::isSphereFrustumVisible(glm::vec3(100,0,0), 1.0f, planes), "frustum side culled");
        // Large sphere that still intersects frustum
        check(HiZPyramid::isSphereFrustumVisible(glm::vec3(0,0,0), 100.0f, planes), "frustum large visible");
        // Edge: sphere exactly on plane (touching)
        // Put sphere just outside far plane
        check(!HiZPyramid::isSphereFrustumVisible(glm::vec3(0,0,-100), 0.1f, planes), "frustum far culled");
        // Test with identity planes (manual): planes that form a cube [-1,1]
        glm::vec4 cubePlanes[6] = {
            glm::vec4( 1,0,0,1), // left x=-1
            glm::vec4(-1,0,0,1), // right x=1
            glm::vec4(0, 1,0,1), // bottom
            glm::vec4(0,-1,0,1), // top
            glm::vec4(0,0, 1,1), // near
            glm::vec4(0,0,-1,1)  // far
        };
        check(HiZPyramid::isSphereFrustumVisible(glm::vec3(0,0,0), 0.5f, cubePlanes), "cube frustum center visible");
        check(!HiZPyramid::isSphereFrustumVisible(glm::vec3(5,0,0), 0.5f, cubePlanes), "cube frustum outside culled");
    }

    // 6. Sphere projection to screen AABB (mirrors instance_cull.comp)
    {
        glm::vec3 camPos{0,0,5};
        glm::mat4 view = glm::lookAt(camPos, glm::vec3(0,0,0), glm::vec3(0,1,0));
        float fov=glm::radians(60.0f), aspect=16.0f/9.0f, zn=0.1f, zf=100.0f;
        glm::mat4 proj = glm::perspectiveRH_ZO(fov, aspect, zn, zf);
        VkExtent2D screen{1920,1080};
        glm::vec2 outMin, outMax, outSize;

        // Sphere at origin (in front of camera) should project to center
        bool ok = HiZPyramid::projectSphereToAABB(glm::vec3(0,0,0), 1.0f, view, proj, screen, zn, outMin, outMax, outSize);
        check(ok, "project center ok");
        check(outMin.x < 0.5f && outMax.x > 0.5f && outMin.y < 0.5f && outMax.y > 0.5f, "project center centered");
        check(outSize.x > 0 && outSize.y > 0, "project center size >0");
        // Larger radius => larger AABB
        glm::vec2 min1, max1, size1, min2, max2, size2;
        HiZPyramid::projectSphereToAABB(glm::vec3(0,0,0), 1.0f, view, proj, screen, zn, min1, max1, size1);
        HiZPyramid::projectSphereToAABB(glm::vec3(0,0,0), 2.0f, view, proj, screen, zn, min2, max2, size2);
        check(size2.x > size1.x && size2.y > size1.y, "project larger radius larger AABB");
        // Farther sphere => smaller AABB
        glm::vec2 minNear, maxNear, sizeNear, minFar, maxFar, sizeFar;
        HiZPyramid::projectSphereToAABB(glm::vec3(0,0,0), 1.0f, view, proj, screen, zn, minNear, maxNear, sizeNear);
        HiZPyramid::projectSphereToAABB(glm::vec3(0,0,-20), 1.0f, view, proj, screen, zn, minFar, maxFar, sizeFar);
        check(sizeFar.x < sizeNear.x, "project farther smaller");
        // Near-plane guard: sphere intersecting near plane should return full screen
        bool nearOk = HiZPyramid::projectSphereToAABB(camPos + glm::vec3(0,0,-0.05f), 1.0f, view, proj, screen, zn, outMin, outMax, outSize);
        check(nearOk && outMin==glm::vec2(0.0f) && outMax==glm::vec2(1.0f), "project near guard full screen");
        // Off-screen sphere (far right) should be clamped to [0,1] and size small or culled?
        bool offOk = HiZPyramid::projectSphereToAABB(glm::vec3(100,0,0), 0.5f, view, proj, screen, zn, outMin, outMax, outSize);
        // It may still produce clamped AABB at edge; we just check it doesn't crash and is clamped
        check(!offOk || (outMin.x >=0 && outMax.x <=1 && outMin.y>=0 && outMax.y<=1), "project offscreen clamped");
    }

    // 7. Mip selection (log2)
    {
        uint32_t maxMip = 10;
        check(HiZPyramid::mipForAABBSize(glm::vec2(1,1), maxMip)==0, "mip 1x1 =>0");
        check(HiZPyramid::mipForAABBSize(glm::vec2(2,2), maxMip)==1, "mip 2x2 =>1");
        check(HiZPyramid::mipForAABBSize(glm::vec2(4,4), maxMip)==2, "mip 4x4 =>2");
        check(HiZPyramid::mipForAABBSize(glm::vec2(100,50), maxMip)==7, "mip 100x50 => ceil log2 100=7");
        check(HiZPyramid::mipForAABBSize(glm::vec2(1024,1024), maxMip)==10, "mip 1024 clamp max");
        check(HiZPyramid::mipForAABBSize(glm::vec2(0,0), maxMip)==0, "mip 0 clamp 0");
        // Max dimension picks larger axis
        check(HiZPyramid::mipForAABBSize(glm::vec2(16,4), maxMip)==4, "mip 16x4 =>4 (max 16)");
    }

    // 8. RenderGraph: HiZ build + occlusion cull + indirect draw
    {
        RenderGraph g;
        VkExtent2D screen{1920,1080};
        // Depth buffer (imported or transient)
        ImageDesc depthDesc; depthDesc.name="Depth"; depthDesc.format=VK_FORMAT_D32_SFLOAT; depthDesc.extent=screen; depthDesc.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        auto depth = g.createResource(depthDesc);
        // Instance buffers (transient)
        BufferDesc instDesc; instDesc.name="AllInstances"; instDesc.size=50000*sizeof(GPUInstanceData); instDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        BufferDesc indirectDesc; indirectDesc.name="Indirect"; indirectDesc.size=32*sizeof(GPUIndirectCommand); indirectDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        BufferDesc visibleDesc; visibleDesc.name="VisibleIndices"; visibleDesc.size=50000*sizeof(uint32_t); visibleDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto allInstances = g.createBuffer(instDesc);
        auto indirect = g.createBuffer(indirectDesc);
        auto visible = g.createBuffer(visibleDesc);

        GPUCullingSystem culling;
        culling.setInstanceCount(50000);
        culling.setBatchCount(32);
        culling.buildCullingPipeline(g, depth, allInstances, indirect, visible, screen);

        check(g.passCount()==3, "culling pipeline 3 passes");
        bool foundHiZ=false, foundCull=false, foundDraw=false;
        for(auto& p: g.passes()){
            if(p.name=="HiZ_Build_Pass"){ foundHiZ=true; check(p.reads.size()==1 && p.writes.size()==1, "HiZ pass 1R1W"); check(p.reads[0].second==ResourceUsage::ShaderRead, "HiZ reads depth ShaderRead"); check(p.writes[0].second==ResourceUsage::ComputeWrite, "HiZ writes ComputeWrite"); }
            if(p.name=="GPU_Occlusion_Cull_Pass"){ foundCull=true; check(p.bufferReads.size()==1, "Cull 1 buffer read (instances)"); check(p.bufferWrites.size()==2, "Cull 2 buffer writes (indirect+visible)"); // hiz is image read
                bool hasHizRead=false; for(auto& [h,u]: p.reads) if(u==ResourceUsage::ShaderRead) hasHizRead=true; check(hasHizRead, "Cull reads HiZ ShaderRead");
                bool hasIndirectWrite=false, hasVisibleWrite=false;
                for(auto& [h,u]: p.bufferWrites) { if(h.id==indirect.id && u==BufferUsage::ComputeWrite) hasIndirectWrite=true; if(h.id==visible.id && u==BufferUsage::ComputeWrite) hasVisibleWrite=true; }
                check(hasIndirectWrite && hasVisibleWrite, "Cull writes indirect+visible ComputeWrite");
                // Check BufferUsage indirect not yet, but draw reads it as IndirectBuffer
            }
            if(p.name=="GBuffer_Indirect_Draw_Pass"){ foundDraw=true; bool hasIndirect=false; for(auto& [h,u]: p.bufferReads) if(h.id==indirect.id && u==BufferUsage::IndirectBuffer) hasIndirect=true; check(hasIndirect, "Draw reads indirect IndirectBuffer"); }
        }
        check(foundHiZ && foundCull && foundDraw, "culling all 3 passes found");

        bool ok = g.compile();
        check(ok, "culling compile true");
        auto& sorted = g.sortedPassIndices();
        auto idxOf = [&](const char* n)->int{ for(size_t i=0;i<sorted.size();++i) if(g.passes()[sorted[i]].name==n) return (int)i; return -1; };
        int hi=idxOf("HiZ_Build_Pass"), cu=idxOf("GPU_Occlusion_Cull_Pass"), dr=idxOf("GBuffer_Indirect_Draw_Pass");
        check(hi>=0 && cu>=0 && dr>=0, "culling sorted found");
        check(hi < cu && cu < dr, "culling order HiZ->Cull->Draw");

        // Barrier safety: HiZ ComputeWrite -> ShaderRead, ComputeWrite -> IndirectBuffer should have barriers
        // Check barrier states
        auto barrierComputeWrite = getBarrierState(ResourceUsage::ComputeWrite);
        check(barrierComputeWrite.stageMask==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT && barrierComputeWrite.accessMask==VK_ACCESS_SHADER_WRITE_BIT && barrierComputeWrite.layout==VK_IMAGE_LAYOUT_GENERAL, "barrier HiZ ComputeWrite GENERAL");
        auto barrierShaderRead = getBarrierState(ResourceUsage::ShaderRead);
        check(barrierShaderRead.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "barrier ShaderRead");
        auto barrierIndirect = getBarrierState(BufferUsage::IndirectBuffer);
        check(barrierIndirect.stageMask==VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT && barrierIndirect.accessMask==VK_ACCESS_INDIRECT_COMMAND_READ_BIT, "barrier IndirectBuffer");
        auto barrierComputeRead = getBarrierState(BufferUsage::ComputeRead);
        check(barrierComputeRead.stageMask==VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, "barrier ComputeRead");

        // Execute with null cmd should not crash, barriers handled
        g.execute(VK_NULL_HANDLE);
        check(true, "culling execute no crash");

        // Verify HiZ resource has mipLevels >1
        bool hizFound=false;
        for(auto& r: g.resources()){
            if(r.name=="HiZ_Pyramid"){ hizFound=true; check(r.desc.mipLevels==11, "HiZ mipLevels 11"); check(r.desc.format==VK_FORMAT_R32_SFLOAT, "HiZ format R32"); check((r.desc.usage & VK_IMAGE_USAGE_STORAGE_BIT) && (r.desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT), "HiZ usage STORAGE|SAMPLED"); check(r.desc.extent.width==960 && r.desc.extent.height==540, "HiZ base extent half"); }
        }
        check(hizFound, "HiZ resource found");

        // Clear wipes
        g.clear();
        check(g.passCount()==0 && g.resourceCount()==0 && g.bufferCount()==0, "culling clear wipes");
    }

    // 9. Integration with TransientResourcePool (mipLevels allocation)
    {
        TransientResourcePool pool(VK_NULL_HANDLE, VK_NULL_HANDLE);
        RenderGraph g;
        VkExtent2D screen{1280,720};
        ImageDesc depthDesc; depthDesc.name="Depth"; depthDesc.format=VK_FORMAT_D32_SFLOAT; depthDesc.extent=screen; depthDesc.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        auto depth = g.createResource(depthDesc);
        BufferDesc instDesc; instDesc.name="Inst"; instDesc.size=100*sizeof(GPUInstanceData); instDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        BufferDesc indirectDesc; indirectDesc.name="Indirect"; indirectDesc.size=4*sizeof(GPUIndirectCommand); indirectDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        BufferDesc visDesc; visDesc.name="Vis"; visDesc.size=100*4; visDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto instances = g.createBuffer(instDesc);
        auto indirect = g.createBuffer(indirectDesc);
        auto visible = g.createBuffer(visDesc);
        GPUCullingSystem sys;
        sys.buildCullingPipeline(g, depth, instances, indirect, visible, screen);
        bool ok = g.compile(0, pool);
        check(ok, "culling pool compile");
        // HiZ should have allocated one physical image with mipLevels 11
        size_t allocs = pool.getTotalAllocationCount();
        check(allocs>=1, "culling pool alloc >=1");
        // Depth and HiZ have different formats/extents, so at least 2 allocs?
        check(allocs>=2, "culling pool alloc depth+HiZ");
    }

    if(failCount==0) printf("PASS: HiZ pyramid, frustum, projection, mip, bounds, culling pipeline\n");
    else printf("FAIL %d checks\n", failCount);
    return failCount==0?0:1;
}
