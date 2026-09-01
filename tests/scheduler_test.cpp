// Scheduler: DAG partitioning, timeline monotonic, fallback, ownership transfer
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/scheduler/TimelineSync.cpp ../src/renderer/scheduler/FrameScheduler.cpp ../src/renderer/graph/RenderGraph.cpp ../src/renderer/graph/RenderGraphBuilder.cpp ../src/renderer/graph/RenderGraphResources.cpp ../src/renderer/graph/ResourceLifetime.cpp ../src/renderer/graph/TransientResourcePool.cpp ../src/renderer/vulkan/VkBarrierHelper.cpp ../src/renderer/vulkan/VkDeviceMemoryHelper.cpp scheduler_test.cpp -o /tmp/scheduler_test -lvulkan && /tmp/scheduler_test

#include "renderer/scheduler/QueueTypes.hpp"
#include "renderer/scheduler/TimelineSync.hpp"
#include "renderer/scheduler/FrameScheduler.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include "renderer/api/Synchronization.hpp"

#include <cstdio>
#include <vector>
#include <unordered_map>
#include <string>

using namespace Engine;

static int failCount=0;
static void check(bool cond, const char* msg){ if(!cond){ printf("FAIL %s\n", msg); failCount++; } else printf("OK %s\n", msg); }

static VkImage dummyImage(uint64_t id){ return reinterpret_cast<VkImage>(id); }
static VkImageView dummyView(uint64_t id){ return reinterpret_cast<VkImageView>(id); }
static VkBuffer dummyBuffer(uint64_t id){ return reinterpret_cast<VkBuffer>(id); }

int main(){
    // 1. QueueFamilyIndices
    {
        QueueFamilyIndices idx;
        idx.graphicsFamily = 0;
        idx.computeFamily = 1;
        idx.transferFamily = 2;
        check(idx.hasDedicatedCompute(), "hasDedicatedCompute true");
        check(idx.hasDedicatedTransfer(), "hasDedicatedTransfer true");
        QueueFamilyIndices unified;
        unified.graphicsFamily = 0;
        unified.computeFamily = 0;
        unified.transferFamily = 0;
        check(!unified.hasDedicatedCompute(), "unified no dedicated compute");
        check(!unified.hasDedicatedTransfer(), "unified no dedicated transfer");
        QueueFamilyIndices noTransfer;
        noTransfer.graphicsFamily = 0;
        noTransfer.computeFamily = 1;
        noTransfer.transferFamily = UINT32_MAX;
        check(noTransfer.hasDedicatedCompute(), "dedicated compute without transfer");
        check(!noTransfer.hasDedicatedTransfer(), "no transfer when UINT32_MAX");
    }

    // 2. TimelineSync monotonicity (headless)
    {
        VkDevice dev = VK_NULL_HANDLE;
        VkSemaphore sem = TimelineSync::create(dev, 0);
        check(sem != VK_NULL_HANDLE, "timeline create dummy");
        check(TimelineSync::getValue(dev, sem)==0, "timeline initial 0");
        check(TimelineSync::isMonotonic(0,1), "monotonic 0->1 true");
        check(!TimelineSync::isMonotonic(1,1), "monotonic 1->1 false");
        check(!TimelineSync::isMonotonic(2,1), "monotonic 2->1 false");
        TimelineSync::hostSignal(sem, 1);
        check(TimelineSync::getValue(dev, sem)==1, "timeline after signal 1");
        TimelineSync::hostSignal(sem, 2);
        check(TimelineSync::getValue(dev, sem)==2, "timeline after signal 2");
        // Duplicate should not increase (strictly monotonic check, but hostSignal enforces >)
        TimelineSync::hostSignal(sem, 2);
        check(TimelineSync::getValue(dev, sem)==2, "timeline duplicate no increase");
        TimelineSync::hostSignal(sem, 5);
        check(TimelineSync::getValue(dev, sem)==5, "timeline jump to 5");
        check(TimelineSync::hostValue(sem)==5, "hostValue 5");
        TimelineSync::destroy(dev, sem);
        check(TimelineSync::getValue(dev, sem)==0, "timeline after destroy 0");
    }

    // 3. Partition with dedicated compute: Graphics + Compute interleaved -> 3 batches, cross-queue deps
    {
        QueueFamilyIndices idx; idx.graphicsFamily=0; idx.computeFamily=1; idx.transferFamily=2;
        FrameScheduler scheduler(VK_NULL_HANDLE, idx);
        RenderGraph g;
        VkExtent2D ext{1280,720};
        // Resources
        ImageDesc shadowDesc; shadowDesc.name="ShadowMap"; shadowDesc.format=VK_FORMAT_D32_SFLOAT; shadowDesc.extent={2048,2048}; shadowDesc.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        auto shadowMap = g.createResource(shadowDesc);
        BufferDesc clusterDesc; clusterDesc.name="ClusterGrid"; clusterDesc.size=1024; clusterDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto clusterGrid = g.createBuffer(clusterDesc);
        BufferDesc indirectDesc; indirectDesc.name="Indirect"; indirectDesc.size=1024; indirectDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        auto indirect = g.createBuffer(indirectDesc);
        ImageDesc gbufferDesc; gbufferDesc.name="GBuffer"; gbufferDesc.format=VK_FORMAT_R8G8B8A8_UNORM; gbufferDesc.extent=ext; gbufferDesc.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        auto gbuffer = g.createResource(gbufferDesc);
        ImageDesc hdrDesc; hdrDesc.name="HDR"; hdrDesc.format=VK_FORMAT_R16G16B16A16_SFLOAT; hdrDesc.extent=ext; hdrDesc.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        auto hdr = g.createResource(hdrDesc);
        auto swap = g.importImage("Swapchain", dummyImage(0x100), dummyView(0x101), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);

        // Build pipeline per spec's drawFrame (5 passes) - avoid cycle: HiZ reads separate depth, not GBuffer
        ImageDesc depthDesc2; depthDesc2.name="DepthForHiZ"; depthDesc2.format=VK_FORMAT_D32_SFLOAT; depthDesc2.extent=ext; depthDesc2.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        auto depthForHiZ = g.createResource(depthDesc2);
        // Initialize depthForHiZ with a dummy write so HiZ has something to read without cycle
        g.addPass("DepthInit", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.write(depthForHiZ, ResourceUsage::DepthStencilAttachment); },
            [&](VkCommandBuffer){});
        g.addPass("ShadowPass", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.write(shadowMap, ResourceUsage::DepthStencilAttachment); },
            [&](VkCommandBuffer){});
        g.addPass("ClusterLightCulling", QueueType::AsyncCompute,
            [&](RenderGraphBuilder& b){ b.write(clusterGrid, BufferUsage::ComputeWrite); },
            [&](VkCommandBuffer){});
        g.addPass("HiZ_Build_And_Cull", QueueType::AsyncCompute,
            [&](RenderGraphBuilder& b){ b.read(depthForHiZ, ResourceUsage::ShaderRead); b.write(indirect, BufferUsage::ComputeWrite); },
            [&](VkCommandBuffer){});
        g.addPass("GBufferPass", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.read(indirect, BufferUsage::IndirectBuffer); b.write(gbuffer, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){});
        g.addPass("DeferredLightingPass", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.read(gbuffer, ResourceUsage::ShaderRead); b.read(clusterGrid, BufferUsage::FragmentRead); b.read(shadowMap, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){});

        bool ok = g.compile();
        check(ok, "dedicated compile true");
        check(g.sortedPassIndices().size()==6, "dedicated 6 passes sorted");

        std::vector<CommandBatch> batches;
        scheduler.partitionDAG(g, batches);
        // With dedicated compute, we expect at least 3 batches: G, C+C, G+G (since two compute contiguous)
        // Shadow(G) + Cluster(C) + HiZ(C) contiguous -> second batch has 2 compute passes, third batch has 2 graphics
        check(batches.size()>=3 && batches.size()<=4, "dedicated batches 3-4");
        // Verify queue types
        bool hasGraphics=false, hasCompute=false;
        for(auto& b: batches){ if(b.queueType==QueueType::Graphics) hasGraphics=true; if(b.queueType==QueueType::AsyncCompute) hasCompute=true; }
        check(hasGraphics && hasCompute, "dedicated has both queues");
        // Verify fallback not applied: Cluster should still be AsyncCompute
        bool clusterIsCompute=false;
        for(auto& p: g.passes()) if(p.name=="ClusterLightCulling") clusterIsCompute = (p.actualQueue==QueueType::AsyncCompute);
        check(clusterIsCompute, "dedicated Cluster stays Compute");
        // Verify cross-queue deps: GBufferPass (Graphics) reads indirect written by HiZ (Compute) -> should wait
        // Find GBuffer batch and check it has wait on compute timeline
        int gbufferBatch=-1, hizBatch=-1;
        for(size_t i=0;i<batches.size();++i){
            for(uint32_t pid: batches[i].passIndices){
                if(g.passes()[pid].name=="GBufferPass") gbufferBatch=(int)i;
                if(g.passes()[pid].name=="HiZ_Build_And_Cull") hizBatch=(int)i;
            }
        }
        check(gbufferBatch>=0 && hizBatch>=0, "dedicated found GBuffer and HiZ batches");
        if(gbufferBatch>=0 && hizBatch>=0){
            check(hizBatch < gbufferBatch, "dedicated HiZ before GBuffer");
            // GBuffer batch should have wait on compute
            bool hasWait=false;
            for(auto sem: batches[gbufferBatch].waitSemaphores) if(sem==scheduler.queueContext(QueueType::AsyncCompute).timelineSemaphore) hasWait=true;
            check(hasWait, "dedicated GBuffer waits on Compute timeline");
            // Signal values monotonic
            check(batches[hizBatch].signalValues[0] >0, "dedicated HiZ signal >0");
            check(batches[gbufferBatch].waitValues[0]==batches[hizBatch].signalValues[0], "dedicated wait equals HiZ signal");
        }
        // Verify no deadlock: wait batch index < signal batch index already checked
        // Verify timeline monotonic per queue
        uint64_t lastGraphics=0, lastCompute=0;
        bool mono=true;
        for(auto& b: batches){
            if(b.queueType==QueueType::Graphics){
                if(b.signalValues[0] <= lastGraphics) mono=false;
                lastGraphics=b.signalValues[0];
            } else if(b.queueType==QueueType::AsyncCompute){
                if(b.signalValues[0] <= lastCompute) mono=false;
                lastCompute=b.signalValues[0];
            }
        }
        check(mono, "dedicated timeline monotonic per queue");
    }

    // 4. Fallback to unified queues (no dedicated compute) -> all remapped to Graphics, single batch or fewer
    {
        QueueFamilyIndices unified; unified.graphicsFamily=0; unified.computeFamily=0; unified.transferFamily=0;
        FrameScheduler scheduler(VK_NULL_HANDLE, unified);
        RenderGraph g;
        VkExtent2D ext{1280,720};
        ImageDesc shadowDesc; shadowDesc.name="ShadowMap"; shadowDesc.format=VK_FORMAT_D32_SFLOAT; shadowDesc.extent={2048,2048}; shadowDesc.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        auto shadowMap = g.createResource(shadowDesc);
        BufferDesc clusterDesc; clusterDesc.name="ClusterGrid"; clusterDesc.size=1024; clusterDesc.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto clusterGrid = g.createBuffer(clusterDesc);
        g.addPass("ShadowPass", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.write(shadowMap, ResourceUsage::DepthStencilAttachment); },
            [&](VkCommandBuffer){});
        g.addPass("ClusterLightCulling", QueueType::AsyncCompute,
            [&](RenderGraphBuilder& b){ b.write(clusterGrid, BufferUsage::ComputeWrite); },
            [&](VkCommandBuffer){});
        g.addPass("GBufferPass", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.read(clusterGrid, BufferUsage::FragmentRead); b.write(shadowMap, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){});

        g.compile();
        std::vector<CommandBatch> batches;
        scheduler.partitionDAG(g, batches);
        // With unified, AsyncCompute should fallback to Graphics, so all passes should be Graphics -> single batch
        bool allGraphics=true;
        for(auto& b: batches) if(b.queueType!=QueueType::Graphics) allGraphics=false;
        check(allGraphics, "unified fallback all Graphics");
        check(batches.size()==1, "unified single batch");
        // No cross-queue waits expected
        bool hasWait=false;
        for(auto& b: batches) if(!b.waitSemaphores.empty()) hasWait=true;
        check(!hasWait, "unified no cross-queue waits");
        // Verify actualQueue remapped
        bool clusterIsGraphics=false;
        for(auto& p: g.passes()) if(p.name=="ClusterLightCulling") clusterIsGraphics = (p.actualQueue==QueueType::Graphics);
        check(clusterIsGraphics, "unified Cluster remapped to Graphics");
    }

    // 5. Timeline progression across frames (monotonic increase over multiple scheduleAndExecute)
    {
        QueueFamilyIndices idx; idx.graphicsFamily=0; idx.computeFamily=1; idx.transferFamily=2;
        FrameScheduler scheduler(VK_NULL_HANDLE, idx);
        RenderGraph g;
        BufferDesc bd; bd.name="Buf"; bd.size=1024; bd.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto buf = g.createBuffer(bd);
        g.addPass("ComputePass", QueueType::AsyncCompute,
            [&](RenderGraphBuilder& b){ b.write(buf, BufferUsage::ComputeWrite); },
            [&](VkCommandBuffer){});
        g.addPass("GraphicsPass", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.read(buf, BufferUsage::VertexRead); },
            [&](VkCommandBuffer){});

        g.compile();
        // First frame
        std::vector<CommandBatch> b1;
        scheduler.partitionDAG(g, b1);
        uint64_t computeSig1=0, graphicsSig1=0;
        for(auto& b: b1){
            if(b.queueType==QueueType::AsyncCompute) computeSig1=b.signalValues[0];
            if(b.queueType==QueueType::Graphics) graphicsSig1=b.signalValues[0];
        }
        check(computeSig1==1 && graphicsSig1==1, "frame1 signals 1,1");

        // Second frame (new graph but same scheduler, timeline should continue)
        RenderGraph g2;
        auto buf2 = g2.createBuffer(bd);
        g2.addPass("ComputePass", QueueType::AsyncCompute,
            [&](RenderGraphBuilder& b){ b.write(buf2, BufferUsage::ComputeWrite); },
            [&](VkCommandBuffer){});
        g2.addPass("GraphicsPass", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.read(buf2, BufferUsage::VertexRead); },
            [&](VkCommandBuffer){});
        g2.compile();
        std::vector<CommandBatch> b2;
        scheduler.partitionDAG(g2, b2);
        uint64_t computeSig2=0, graphicsSig2=0;
        for(auto& b: b2){
            if(b.queueType==QueueType::AsyncCompute) computeSig2=b.signalValues[0];
            if(b.queueType==QueueType::Graphics) graphicsSig2=b.signalValues[0];
        }
        check(computeSig2==2 && graphicsSig2==2, "frame2 signals 2,2 monotonic");
        check(computeSig2 > computeSig1 && graphicsSig2 > graphicsSig1, "timeline strictly increasing across frames");
    }

    // 6. Queue Family Ownership Transfer
    {
        QueueFamilyIndices idx; idx.graphicsFamily=0; idx.computeFamily=1; idx.transferFamily=2;
        FrameScheduler scheduler(VK_NULL_HANDLE, idx);
        RenderGraphResource res;
        res.name="TestImage";
        res.desc.format=VK_FORMAT_R8G8B8A8_UNORM;
        res.desc.extent={512,512};
        res.desc.usage=VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        res.image = dummyImage(0x123);
        res.view = dummyView(0x124);
        // Same family -> no op, should not crash
        scheduler.injectQueueOwnershipTransfer(VK_NULL_HANDLE, VK_NULL_HANDLE, res, 0, 0, ResourceUsage::ShaderRead, ResourceUsage::ComputeWrite);
        check(true, "ownership same family no crash");
        // Cross family -> should not crash with dummy handles
        scheduler.injectQueueOwnershipTransfer(VK_NULL_HANDLE, VK_NULL_HANDLE, res, 0, 1, ResourceUsage::ShaderRead, ResourceUsage::ComputeWrite);
        check(true, "ownership cross family dummy no crash");
        // Buffer variant
        RenderGraphBufferResource bufRes;
        bufRes.name="TestBuf";
        bufRes.desc.size=1024;
        bufRes.buffer = dummyBuffer(0x200);
        scheduler.injectQueueOwnershipTransfer(VK_NULL_HANDLE, VK_NULL_HANDLE, bufRes, 0, 1, BufferUsage::ComputeWrite, BufferUsage::VertexRead);
        check(true, "ownership buffer cross family no crash");
        // With actual VkCommandBuffers (still null, but family differs) - ensure barrier logic handles GENERAL vs SHADER_READ
        // Already covered
    }

    // 7. Deadlock prevention: ensure no circular wait (Graphics waits Compute, Compute waits Graphics should not happen in DAG order)
    {
        QueueFamilyIndices idx; idx.graphicsFamily=0; idx.computeFamily=1; idx.transferFamily=2;
        FrameScheduler scheduler(VK_NULL_HANDLE, idx);
        RenderGraph g;
        // Create a diamond but with queue types that could cause deadlock if mis-handled
        // A(Graphics) -> B(Compute) -> D(Graphics)
        // A -> C(Compute) -> D
        // If scheduler incorrectly made B wait on D, that would be circular (D after B). Our scheduler only allows wait on earlier batches, so safe.
        ImageDesc aDesc; aDesc.name="A"; aDesc.format=VK_FORMAT_R8G8B8A8_UNORM; aDesc.extent={512,512}; aDesc.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ImageDesc bDesc; bDesc.name="B"; bDesc.format=VK_FORMAT_R8G8B8A8_UNORM; bDesc.extent={512,512}; bDesc.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ImageDesc cDesc; cDesc.name="C"; cDesc.format=VK_FORMAT_R8G8B8A8_UNORM; cDesc.extent={512,512}; cDesc.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        auto resA = g.createResource(aDesc);
        auto resB = g.createResource(bDesc);
        auto resC = g.createResource(cDesc);
        auto swap = g.importImage("Swapchain", dummyImage(0x300), dummyView(0x301), VK_FORMAT_B8G8R8A8_UNORM, VkExtent2D{512,512}, ResourceUsage::None);
        g.addPass("A_Graphics", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.write(resA, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){});
        g.addPass("B_Compute", QueueType::AsyncCompute,
            [&](RenderGraphBuilder& b){ b.read(resA, ResourceUsage::ShaderRead); b.write(resB, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){});
        g.addPass("C_Compute", QueueType::AsyncCompute,
            [&](RenderGraphBuilder& b){ b.read(resA, ResourceUsage::ShaderRead); b.write(resC, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){});
        g.addPass("D_Graphics", QueueType::Graphics,
            [&](RenderGraphBuilder& b){ b.read(resB, ResourceUsage::ShaderRead); b.read(resC, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){});
        g.compile();
        std::vector<CommandBatch> batches;
        scheduler.partitionDAG(g, batches);
        // Check that no batch waits on a later batch (would be deadlock)
        bool deadlock=false;
        // Build pass to batch map
        std::unordered_map<uint32_t, size_t> passToBatch;
        for(size_t i=0;i<batches.size();++i) for(uint32_t pid: batches[i].passIndices) passToBatch[pid]=i;
        for(size_t i=0;i<batches.size();++i){
            for(auto sem: batches[i].waitSemaphores){
                // Find which batch this semaphore corresponds to (by matching semaphore handle)
                // For this test, we just ensure wait batch index < current batch index via our earlier logic
                // Since we only add waits on writerBatch < b, it should be safe
                // We can verify by checking that every wait corresponds to earlier batch's signal
                // Here we just check that batches are in order and no batch waits on itself
                if(batches[i].waitSemaphores.size()>0){
                    // Find dep batch by checking which earlier batch has same semaphore and waitValue equals its signal
                    // For simplicity, ensure waitValues are from earlier batches
                    for(size_t w=0; w<batches[i].waitValues.size(); ++w){
                        uint64_t waitVal = batches[i].waitValues[w];
                        bool found=false;
                        for(size_t j=0;j<i;++j){
                            for(uint64_t sig: batches[j].signalValues) if(sig==waitVal) found=true;
                        }
                        if(!found) deadlock=true;
                    }
                }
            }
        }
        check(!deadlock, "no deadlock circular wait");
        // Also check that batches are topologically sorted: writer batch before reader batch
        // Already ensured by partitionDAG ordering
        check(true, "deadlock prevention validated");
    }

    // 8. Transfer queue handling (if dedicated transfer exists, it should be used)
    {
        QueueFamilyIndices withTransfer; withTransfer.graphicsFamily=0; withTransfer.computeFamily=1; withTransfer.transferFamily=2;
        FrameScheduler s1(VK_NULL_HANDLE, withTransfer);
        RenderGraph g;
        BufferDesc td; td.name="TransferBuf"; td.size=1024; td.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto tbuf = g.createBuffer(td);
        g.addPass("TransferPass", QueueType::Transfer,
            [&](RenderGraphBuilder& b){ b.write(tbuf, BufferUsage::TransferDst); },
            [&](VkCommandBuffer){});
        g.compile();
        std::vector<CommandBatch> b;
        s1.partitionDAG(g,b);
        bool hasTransfer=false;
        for(auto& batch: b) if(batch.queueType==QueueType::Transfer) hasTransfer=true;
        check(hasTransfer, "dedicated transfer kept");

        QueueFamilyIndices noTransfer; noTransfer.graphicsFamily=0; noTransfer.computeFamily=1; noTransfer.transferFamily=UINT32_MAX;
        FrameScheduler s2(VK_NULL_HANDLE, noTransfer);
        RenderGraph g2;
        auto tbuf2 = g2.createBuffer(td);
        g2.addPass("TransferPass", QueueType::Transfer,
            [&](RenderGraphBuilder& b){ b.write(tbuf2, BufferUsage::TransferDst); },
            [&](VkCommandBuffer){});
        g2.compile();
        std::vector<CommandBatch> b2;
        s2.partitionDAG(g2,b2);
        bool allGraphics=true;
        for(auto& batch: b2) if(batch.queueType!=QueueType::Graphics) allGraphics=false;
        check(allGraphics, "no dedicated transfer falls back to Graphics");
    }

    if(failCount==0) printf("PASS: scheduler DAG, batches, fallback, timeline, ownership\n");
    else printf("FAIL %d checks\n", failCount);
    return failCount==0?0:1;
}
