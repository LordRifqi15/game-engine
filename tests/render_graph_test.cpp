// Render Graph: topological sort, cycle detection, barrier tests
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/graph/RenderGraph.cpp ../src/renderer/graph/RenderGraphBuilder.cpp ../src/renderer/graph/RenderGraphResources.cpp ../src/renderer/vulkan/VkBarrierHelper.cpp render_graph_test.cpp ../src/renderer/graph/TransientResourcePool.cpp ../src/renderer/graph/ResourceLifetime.cpp ../src/renderer/vulkan/VkDeviceMemoryHelper.cpp -o /tmp/render_graph_test -lvulkan && /tmp/render_graph_test

#include "renderer/graph/RenderGraph.hpp"
#include "renderer/graph/RenderGraphBuilder.hpp"
#include "renderer/graph/RenderGraphResources.hpp"
#include "renderer/api/Synchronization.hpp"

#include <cstdio>
#include <vector>
#include <string>

using namespace Engine;

static int failCount = 0;
static void check(bool cond, const char* msg) {
    if (!cond) { printf("FAIL %s\n", msg); failCount++; }
    else { printf("OK %s\n", msg); }
}

static VkImage dummyImage(uint64_t id) { return reinterpret_cast<VkImage>(id); }
static VkImageView dummyView(uint64_t id) { return reinterpret_cast<VkImageView>(id); }

int main() {
    // 1. Linear dependency: Shadow -> Main -> Present (spec pipeline)
    {
        RenderGraph graph;
        VkExtent2D ext{1280,720};
        auto swap = graph.importImage("Swapchain", dummyImage(0x100), dummyView(0x101), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        ImageDesc shadowDesc; shadowDesc.name="ShadowMap"; shadowDesc.format=VK_FORMAT_D32_SFLOAT; shadowDesc.extent={2048,2048}; shadowDesc.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        auto shadowMap = graph.createResource(shadowDesc);
        std::vector<std::string> execOrder;
        graph.addPass("ShadowPass",
            [&](RenderGraphBuilder& b){ b.write(shadowMap, ResourceUsage::DepthStencilAttachment); },
            [&](VkCommandBuffer){ execOrder.push_back("ShadowPass"); }
        );
        graph.addPass("MainForwardPass",
            [&](RenderGraphBuilder& b){ b.read(shadowMap, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); },
            [&](VkCommandBuffer){ execOrder.push_back("MainForwardPass"); }
        );
        graph.addPass("PresentPass",
            [&](RenderGraphBuilder& b){ b.read(swap, ResourceUsage::Present); },
            [&](VkCommandBuffer){ execOrder.push_back("PresentPass"); }
        );
        bool ok = graph.compile();
        check(ok, "linear compile true");
        check(graph.sortedPassIndices().size()==3, "linear 3 passes kept");
        // verify order: Shadow before Main before Present in sorted order
        auto& sorted = graph.sortedPassIndices();
        auto& passes = graph.passes();
        int shadowIdx=-1, mainIdx=-1, presentIdx=-1;
        for (size_t i=0;i<sorted.size();++i){
            auto& p = passes[sorted[i]];
            if (p.name=="ShadowPass") shadowIdx=(int)i;
            if (p.name=="MainForwardPass") mainIdx=(int)i;
            if (p.name=="PresentPass") presentIdx=(int)i;
        }
        check(shadowIdx>=0 && mainIdx>=0 && presentIdx>=0, "linear all passes found");
        check(shadowIdx < mainIdx && mainIdx < presentIdx, "linear Shadow->Main->Present order");
        // execute and verify callback order
        execOrder.clear();
        graph.execute(VK_NULL_HANDLE);
        check(execOrder.size()==3 && execOrder[0]=="ShadowPass" && execOrder[1]=="MainForwardPass" && execOrder[2]=="PresentPass", "linear execute order");
    }

    // 2. Dynamic topology: reordering addPass declarations yields same DAG order
    {
        auto buildGraph = [](bool shuffled)->std::vector<std::string> {
            RenderGraph g;
            VkExtent2D ext{1280,720};
            auto swap = g.importImage("Swapchain", dummyImage(0x200), dummyView(0x201), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
            ImageDesc d; d.name="ShadowMap"; d.format=VK_FORMAT_D32_SFLOAT; d.extent={2048,2048}; d.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            auto shadow = g.createResource(d);
            std::vector<std::string> order;
            if (!shuffled) {
                g.addPass("ShadowPass", [&](RenderGraphBuilder& b){ b.write(shadow, ResourceUsage::DepthStencilAttachment); }, [&](VkCommandBuffer){});
                g.addPass("MainForwardPass", [&](RenderGraphBuilder& b){ b.read(shadow, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
                g.addPass("PresentPass", [&](RenderGraphBuilder& b){ b.read(swap, ResourceUsage::Present); }, [&](VkCommandBuffer){});
            } else {
                // shuffled declaration: Main, Present, Shadow
                g.addPass("MainForwardPass", [&](RenderGraphBuilder& b){ b.read(shadow, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
                g.addPass("PresentPass", [&](RenderGraphBuilder& b){ b.read(swap, ResourceUsage::Present); }, [&](VkCommandBuffer){});
                g.addPass("ShadowPass", [&](RenderGraphBuilder& b){ b.write(shadow, ResourceUsage::DepthStencilAttachment); }, [&](VkCommandBuffer){});
            }
            g.compile();
            for (auto idx : g.sortedPassIndices()) order.push_back(g.passes()[idx].name);
            return order;
        };
        auto orderA = buildGraph(false);
        auto orderB = buildGraph(true);
        check(orderA.size()==3 && orderB.size()==3, "dynamic topo sizes");
        // both should be Shadow, Main, Present regardless of declaration
        bool same = (orderA==orderB) && orderA[0]=="ShadowPass" && orderA[1]=="MainForwardPass" && orderA[2]=="PresentPass";
        check(same, "dynamic topology identical order");
        if (!same) {
            printf("  orderA: "); for(auto& s:orderA) printf("%s ",s.c_str()); printf("\n");
            printf("  orderB: "); for(auto& s:orderB) printf("%s ",s.c_str()); printf("\n");
        }
    }

    // 3. Branching pipeline: A -> B, A -> C, B+C -> D (diamond)
    {
        RenderGraph g;
        ImageDesc descA; descA.name="A"; descA.format=VK_FORMAT_R8G8B8A8_UNORM; descA.extent={512,512};
        ImageDesc descB; descB.name="B"; descB.format=VK_FORMAT_R8G8B8A8_UNORM; descB.extent={512,512};
        ImageDesc descC; descC.name="C"; descC.format=VK_FORMAT_R8G8B8A8_UNORM; descC.extent={512,512};
        auto resA = g.createResource(descA);
        auto resB = g.createResource(descB);
        auto resC = g.createResource(descC);
        VkExtent2D ext{512,512};
        auto swap = g.importImage("Swapchain", dummyImage(0x300), dummyView(0x301), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        g.addPass("PassA", [&](RenderGraphBuilder& b){ b.write(resA, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("PassB", [&](RenderGraphBuilder& b){ b.read(resA, ResourceUsage::ShaderRead); b.write(resB, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("PassC", [&](RenderGraphBuilder& b){ b.read(resA, ResourceUsage::ShaderRead); b.write(resC, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("PassD", [&](RenderGraphBuilder& b){ b.read(resB, ResourceUsage::ShaderRead); b.read(resC, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("Present", [&](RenderGraphBuilder& b){ b.read(swap, ResourceUsage::Present); }, [&](VkCommandBuffer){});
        bool ok=g.compile();
        check(ok, "branching compile true");
        auto& sorted = g.sortedPassIndices();
        check(sorted.size()==5, "branching 5 passes");
        auto& passes = g.passes();
        auto idxOf = [&](const char* name)->int{
            for(size_t i=0;i<sorted.size();++i) if(passes[sorted[i]].name==name) return (int)i;
            return -1;
        };
        int a=idxOf("PassA"), b=idxOf("PassB"), c=idxOf("PassC"), d=idxOf("PassD"), p=idxOf("Present");
        check(a>=0 && b>=0 && c>=0 && d>=0 && p>=0, "branching all found");
        check(a < b && a < c && b < d && c < d && d < p, "branching diamond order");
    }

    // 4. Dead pass culling
    {
        RenderGraph g;
        VkExtent2D ext{512,512};
        auto swap = g.importImage("Swapchain", dummyImage(0x400), dummyView(0x401), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        ImageDesc shadowDesc; shadowDesc.name="ShadowMap"; shadowDesc.format=VK_FORMAT_D32_SFLOAT; shadowDesc.extent={2048,2048};
        auto shadow = g.createResource(shadowDesc);
        ImageDesc deadDesc; deadDesc.name="DeadTex"; deadDesc.format=VK_FORMAT_R8G8B8A8_UNORM; deadDesc.extent={256,256};
        auto dead = g.createResource(deadDesc);
        g.addPass("ShadowPass", [&](RenderGraphBuilder& b){ b.write(shadow, ResourceUsage::DepthStencilAttachment); }, [&](VkCommandBuffer){});
        g.addPass("MainPass", [&](RenderGraphBuilder& b){ b.read(shadow, ResourceUsage::ShaderRead); b.write(swap, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("PresentPass", [&](RenderGraphBuilder& b){ b.read(swap, ResourceUsage::Present); }, [&](VkCommandBuffer){});
        g.addPass("DeadPass", [&](RenderGraphBuilder& b){ b.write(dead, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        check(g.passCount()==4, "dead: 4 declared");
        bool ok=g.compile();
        check(ok, "dead compile true");
        check(g.sortedPassIndices().size()==3, "dead pruned to 3");
        bool deadFound=false;
        for(auto idx:g.sortedPassIndices()) if(g.passes()[idx].name=="DeadPass") deadFound=true;
        check(!deadFound, "dead pass culled");
    }

    // 5. Cycle detection: A reads B, B reads A -> fail
    {
        RenderGraph g;
        ImageDesc da; da.name="TexA"; da.format=VK_FORMAT_R8G8B8A8_UNORM; da.extent={256,256};
        ImageDesc db; db.name="TexB"; db.format=VK_FORMAT_R8G8B8A8_UNORM; db.extent={256,256};
        auto texA = g.createResource(da);
        auto texB = g.createResource(db);
        g.addPass("PassA", [&](RenderGraphBuilder& b){ b.read(texB, ResourceUsage::ShaderRead); b.write(texA, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("PassB", [&](RenderGraphBuilder& b){ b.read(texA, ResourceUsage::ShaderRead); b.write(texB, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        bool ok=g.compile();
        check(!ok, "cycle detected (should fail)");
        check(g.sortedPassIndices().empty(), "cycle sorted empty");
    }

    // 6. Barrier derivation / synchronization helpers
    {
        auto s = getBarrierState(ResourceUsage::ColorAttachment);
        check(s.stageMask==VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT && s.accessMask==VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT && s.layout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, "barrier ColorAttachment");
        auto d = getBarrierState(ResourceUsage::DepthStencilAttachment);
        check((d.stageMask & (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT))!=0 && d.accessMask==VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT && d.layout==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, "barrier DepthStencil");
        auto r = getBarrierState(ResourceUsage::ShaderRead);
        check(r.stageMask==VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT && r.accessMask==VK_ACCESS_SHADER_READ_BIT && r.layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, "barrier ShaderRead");
        auto p = getBarrierState(ResourceUsage::Present);
        check(p.stageMask==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT && p.layout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, "barrier Present");
        auto tsrc = getBarrierState(ResourceUsage::TransferSrc);
        check(tsrc.layout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, "barrier TransferSrc");
        auto tdst = getBarrierState(ResourceUsage::TransferDst);
        check(tdst.layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, "barrier TransferDst");
        auto none = getBarrierState(ResourceUsage::None);
        check(none.layout==VK_IMAGE_LAYOUT_UNDEFINED, "barrier None");
        // aspect mask
        VkImageAspectFlags col = getAspectMask(VK_FORMAT_R8G8B8A8_UNORM, ResourceUsage::ColorAttachment);
        check(col==VK_IMAGE_ASPECT_COLOR_BIT, "aspect color");
        VkImageAspectFlags depth = getAspectMask(VK_FORMAT_D32_SFLOAT, ResourceUsage::DepthStencilAttachment);
        check(depth==VK_IMAGE_ASPECT_DEPTH_BIT, "aspect depth D32");
        VkImageAspectFlags depth2 = getAspectMask(VK_FORMAT_D24_UNORM_S8_UINT, ResourceUsage::DepthStencilAttachment);
        check(depth2==VK_IMAGE_ASPECT_DEPTH_BIT, "aspect depth D24S8");
    }

    // 7. Transient vs Imported lifetime + barrier transition + lastUsage
    {
        RenderGraph g;
        VkExtent2D ext{512,512};
        auto swap = g.importImage("Swapchain", dummyImage(0x500), dummyView(0x501), VK_FORMAT_B8G8R8A8_UNORM, ext, ResourceUsage::None);
        ImageDesc td; td.name="Transient"; td.format=VK_FORMAT_R8G8B8A8_UNORM; td.extent=ext; td.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        auto trans = g.createResource(td);
        // verify types
        check(g.resources()[trans.id].type==ResourceType::Transient, "transient type");
        check(g.resources()[swap.id].type==ResourceType::Imported, "imported type");
        check(g.resources()[swap.id].lastUsage==ResourceUsage::None, "import initial None");
        g.addPass("Writer", [&](RenderGraphBuilder& b){ b.write(trans, ResourceUsage::ColorAttachment); b.write(swap, ResourceUsage::ColorAttachment); }, [&](VkCommandBuffer){});
        g.addPass("Reader", [&](RenderGraphBuilder& b){ b.read(trans, ResourceUsage::ShaderRead); b.read(swap, ResourceUsage::Present); }, [&](VkCommandBuffer){});
        bool ok=g.compile();
        check(ok, "lifetime compile true");
        // before execute, lastUsage should be None for transient, None for swap
        check(g.resources()[trans.id].lastUsage==ResourceUsage::None, "before execute transient None");
        g.execute(VK_NULL_HANDLE); // no real cmd buffer, should still update lastUsage
        check(g.resources()[trans.id].lastUsage==ResourceUsage::ShaderRead, "after execute transient ShaderRead");
        check(g.resources()[swap.id].lastUsage==ResourceUsage::Present, "after execute swap Present");
        // clear should wipe
        g.clear();
        check(g.passCount()==0 && g.resourceCount()==0, "clear wipes");
    }

    // 8. clear() transient wipe for next frame
    {
        RenderGraph g;
        ImageDesc d; d.name="T"; d.format=VK_FORMAT_R8G8B8A8_UNORM; d.extent={256,256};
        g.createResource(d);
        g.addPass("P", [&](RenderGraphBuilder& b){ (void)b; }, [&](VkCommandBuffer){});
        g.compile();
        g.clear();
        check(g.resourceCount()==0 && g.passCount()==0, "clear second check");
        // reuse after clear
        ImageDesc d2; d2.name="T2"; d2.format=VK_FORMAT_R8G8B8A8_UNORM; d2.extent={256,256};
        auto h = g.createResource(d2);
        check(h.isValid() && h.id==0, "reuse after clear");
    }

    if (failCount==0) printf("PASS: render graph topo, cycles, dead cull, barriers, lifetime\n");
    else printf("FAIL %d checks\n", failCount);
    return failCount==0 ? 0 : 1;
}
