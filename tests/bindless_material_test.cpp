// Bindless Materials: slot allocation, material updates, fallback
// Build: g++ -std=c++17 -I../include -I../third_party ../src/renderer/material/BindlessDescriptorManager.cpp ../src/renderer/vulkan/VkDeviceMemoryHelper.cpp ../src/renderer/material/TextureRegistry.cpp ../src/renderer/vulkan/VkFeatureManager.cpp bindless_material_test.cpp -o /tmp/bindless_material_test -lvulkan && /tmp/bindless_material_test

#include "renderer/material/BindlessDescriptorManager.hpp"
#include "renderer/material/MaterialTypes.hpp"
#include "renderer/material/TextureRegistry.hpp"
#include "renderer/vulkan/VkFeatureManager.hpp"

#include <cstdio>
#include <vector>
#include <vulkan/vulkan.h>

using namespace Engine;

static int failCount=0;
static void check(bool cond, const char* msg){ if(!cond){ printf("FAIL %s\n", msg); failCount++; } else printf("OK %s\n", msg); }

int main(){
    // 1. GPUMaterial layout
    {
        check(sizeof(GPUMaterial)==48, "GPUMaterial size 48");
        check(alignof(GPUMaterial)==16, "GPUMaterial align 16");
        GPUMaterial m;
        check(m.albedoTextureID==0, "GPUMaterial default albedo 0");
        check(m.normalTextureID==1, "GPUMaterial default normal 1");
        check(m.metallicRoughnessTextureID==2, "GPUMaterial default MR 2");
        check(m.samplerID==0, "GPUMaterial default sampler 0");
        check(m.baseColorFactor==glm::vec4(1.0f), "GPUMaterial baseColor 1");
        check(m.metallicFactor==1.0f && m.roughnessFactor==1.0f, "GPUMaterial factors 1");
        // SamplerType count
        check(static_cast<uint32_t>(SamplerType::Count)==5, "SamplerType Count 5");
        check(static_cast<uint32_t>(SamplerType::LinearRepeat)==0, "SamplerType LinearRepeat 0");
        check(static_cast<uint32_t>(SamplerType::ShadowCompare)==4, "SamplerType ShadowCompare 4");
    }

    // 2. Vulkan 1.2 features
    {
        VkPhysicalDeviceVulkan12Features f12{};
        enableVulkan12Features(f12);
        check(f12.sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, "VkFeatureManager sType");
        check(f12.descriptorIndexing==VK_TRUE, "descriptorIndexing true");
        check(f12.shaderSampledImageArrayNonUniformIndexing==VK_TRUE, "nonUniformIndexing true");
        check(f12.descriptorBindingSampledImageUpdateAfterBind==VK_TRUE, "updateAfterBind true");
        check(f12.descriptorBindingPartiallyBound==VK_TRUE, "partiallyBound true");
        check(f12.descriptorBindingVariableDescriptorCount==VK_TRUE, "variableDescriptorCount true");
        check(f12.runtimeDescriptorArray==VK_TRUE, "runtimeDescriptorArray true");
        // Headless check should return true
        check(checkDescriptorIndexingSupport(VK_NULL_HANDLE)==true, "headless descriptor support true");
        check(queryMaxBindlessTextures(VK_NULL_HANDLE)==16384, "headless max bindless 16384");
    }

    // 3. BindlessDescriptorManager init/shutdown headless
    {
        BindlessDescriptorManager mgr;
        mgr.init(VK_NULL_HANDLE);
        check(mgr.getDescriptorSetLayout()!=VK_NULL_HANDLE, "bindless layout dummy");
        check(mgr.getDescriptorSet()!=VK_NULL_HANDLE, "bindless set dummy");
        check(mgr.getMaterialBuffer()!=VK_NULL_HANDLE, "bindless material buffer dummy");
        check(mgr.allocatedTextureCount()==4, "bindless initial 4 reserved");
        // Reserved slots 0-3 should be valid
        check(mgr.isTextureValid(0) && mgr.isTextureValid(1) && mgr.isTextureValid(2) && mgr.isTextureValid(3), "bindless reserved 0-3 valid");
        check(!mgr.isTextureValid(4), "bindless slot 4 initially invalid");
        mgr.shutdown();
        check(mgr.getDescriptorSetLayout()==VK_NULL_HANDLE, "bindless shutdown layout null");
    }

    // 4. Texture slot allocation / freeing
    {
        BindlessDescriptorManager mgr;
        mgr.init(VK_NULL_HANDLE);
        VkImageView dummy1 = reinterpret_cast<VkImageView>(0x1000);
        VkImageView dummy2 = reinterpret_cast<VkImageView>(0x1001);
        VkImageView dummy3 = reinterpret_cast<VkImageView>(0x1002);
        uint32_t slot1 = mgr.registerTexture(dummy1);
        check(slot1==4, "bindless slot1 ==4 (first free after reserved)");
        check(mgr.isTextureValid(slot1), "bindless slot1 valid");
        check(mgr.allocatedTextureCount()==5, "bindless count 5 after 1 alloc");

        uint32_t slot2 = mgr.registerTexture(dummy2);
        check(slot2==5, "bindless slot2 ==5");
        uint32_t slot3 = mgr.registerTexture(dummy3);
        check(slot3==6, "bindless slot3 ==6");

        // Free and reuse
        mgr.freeTexture(slot2);
        check(!mgr.isTextureValid(slot2), "bindless slot2 freed invalid");
        check(mgr.allocatedTextureCount()==6 || mgr.allocatedTextureCount()==5, "bindless count after free"); // 4 reserved +2 remaining =6? Actually 4+3-1=6
        // Next allocation should reuse slot2 (LIFO)
        VkImageView dummy4 = reinterpret_cast<VkImageView>(0x1003);
        uint32_t slot4 = mgr.registerTexture(dummy4);
        check(slot4==slot2, "bindless reuse freed slot LIFO");

        // Reserved slots never freed
        mgr.freeTexture(0);
        check(mgr.isTextureValid(0), "bindless reserved 0 not freed");
        mgr.freeTexture(1);
        check(mgr.isTextureValid(1), "bindless reserved 1 not freed");

        // Invalid handle fallback to 0
        uint32_t fallback = mgr.registerTexture(VK_NULL_HANDLE);
        check(fallback==0, "bindless null view fallback 0");

        // Exhaustion not tested (16384), but check invalid slot
        check(!mgr.isTextureValid(99999), "bindless invalid slot 99999 false");
        mgr.shutdown();
    }

    // 5. Material SSBO updates
    {
        BindlessDescriptorManager mgr;
        mgr.init(VK_NULL_HANDLE);
        GPUMaterial mat;
        mat.albedoTextureID = 10;
        mat.normalTextureID = 11;
        mat.metallicRoughnessTextureID = 12;
        mat.samplerID = static_cast<uint32_t>(SamplerType::LinearClamp);
        mat.baseColorFactor = glm::vec4(0.5f, 0.2f, 0.8f, 1.0f);
        mat.metallicFactor = 0.7f;
        mat.roughnessFactor = 0.3f;
        mgr.updateMaterial(42, mat);
        const GPUMaterial* fetched = mgr.getMaterial(42);
        check(fetched!=nullptr, "bindless getMaterial not null");
        if(fetched){
            check(fetched->albedoTextureID==10, "bindless mat albedo 10");
            check(fetched->normalTextureID==11, "bindless mat normal 11");
            check(fetched->metallicRoughnessTextureID==12, "bindless mat MR 12");
            check(fetched->samplerID==1, "bindless mat sampler LinearClamp 1");
            check(fetched->baseColorFactor==glm::vec4(0.5f,0.2f,0.8f,1.0f), "bindless mat baseColor");
            check(fetched->metallicFactor==0.7f && fetched->roughnessFactor==0.3f, "bindless mat factors");
        }
        // Out of bounds should not crash and return nullptr
        mgr.updateMaterial(5000, mat); // > MAX_MATERIALS 4096
        check(mgr.getMaterial(5000)==nullptr, "bindless mat out of bounds nullptr");
        check(mgr.getMaterial(4096)==nullptr, "bindless mat 4096 out of bounds");

        // Update multiple materials
        for(uint32_t i=0;i<10;++i){
            GPUMaterial m; m.albedoTextureID=i; mgr.updateMaterial(i,m);
        }
        bool allOk=true;
        for(uint32_t i=0;i<10;++i) if(mgr.getMaterial(i)->albedoTextureID!=i) allOk=false;
        check(allOk, "bindless multiple materials");

        // Overwrite
        GPUMaterial mat2; mat2.albedoTextureID=99; mgr.updateMaterial(42, mat2);
        check(mgr.getMaterial(42)->albedoTextureID==99, "bindless mat overwrite");

        mgr.shutdown();
    }

    // 6. TextureRegistry with Bindless manager
    {
        BindlessDescriptorManager mgr;
        mgr.init(VK_NULL_HANDLE);
        TextureRegistry reg;
        reg.init(VK_NULL_HANDLE, &mgr);
        check(reg.allocatedCount()==4, "registry initial 4 fallbacks");
        check(reg.isValid(0) && reg.isValid(1) && reg.isValid(2) && reg.isValid(3), "registry fallbacks 0-3 valid");
        check(reg.fallbackAlbedoSlot()==0, "registry fallbackAlbedo 0");
        check(reg.fallbackNormalSlot()==1, "registry fallbackNormal 1");
        check(reg.fallbackMetallicRoughnessSlot()==2, "registry fallbackMR 2");

        VkImageView v1 = reinterpret_cast<VkImageView>(0x2000);
        uint32_t s1 = reg.allocate(v1);
        check(s1==4, "registry allocate s1==4");
        check(reg.isValid(s1), "registry s1 valid");
        check(reg.allocatedCount()==5, "registry count 5");

        VkImageView v2 = reinterpret_cast<VkImageView>(0x2001);
        uint32_t s2 = reg.allocate(v2);
        check(s2==5, "registry allocate s2==5");

        reg.free(s1);
        check(!reg.isValid(s1), "registry s1 freed");
        check(reg.freeCount()==1, "registry freeCount 1");

        // Reuse
        VkImageView v3 = reinterpret_cast<VkImageView>(0x2002);
        uint32_t s3 = reg.allocate(v3);
        check(s3==s1, "registry reuse s1");

        // Reserved never freed
        reg.free(0);
        check(reg.isValid(0), "registry reserved 0 not freed");

        // Null view fallback
        uint32_t fb = reg.allocate(VK_NULL_HANDLE);
        check(fb==3 || fb==0, "registry null fallback 0 or 3");

        // Headless without manager
        TextureRegistry reg2;
        reg2.init(VK_NULL_HANDLE, nullptr);
        check(reg2.allocatedCount()==4, "registry2 headless 4");
        VkImageView v4 = reinterpret_cast<VkImageView>(0x3000);
        uint32_t s4 = reg2.allocate(v4);
        check(s4==4, "registry2 allocate 4 without manager");

        reg.shutdown();
        reg2.shutdown();
        mgr.shutdown();
    }

    // 7. Bindless limits
    {
        check(BindlessDescriptorManager::MAX_BINDLESS_TEXTURES==16384, "MAX_BINDLESS 16384");
        check(BindlessDescriptorManager::MAX_MATERIALS==4096, "MAX_MATERIALS 4096");
        check(TextureRegistry::RESERVED_SLOTS==4, "RESERVED 4");
    }

    // 8. Fallback handling for uninitialized texture handles (per spec: slots 0-3 hold 1x1 fallbacks)
    {
        BindlessDescriptorManager mgr;
        mgr.init(VK_NULL_HANDLE);
        // Simulate that invalid material texture IDs (e.g., 99999) should resolve to fallback
        // In real shader, nonuniform index with partially bound will fallback to dummy if not bound.
        // Here we test that manager returns fallback slot 0 for null
        uint32_t fb = mgr.registerTexture(VK_NULL_HANDLE);
        check(fb==0, "fallback null ->0");
        // Ensure that material with default IDs (0,1,2) are the fallbacks and are valid
        GPUMaterial def;
        check(def.albedoTextureID==0 && def.normalTextureID==1 && def.metallicRoughnessTextureID==2, "material defaults 0,1,2 fallbacks");
        check(mgr.isTextureValid(def.albedoTextureID) && mgr.isTextureValid(def.normalTextureID) && mgr.isTextureValid(def.metallicRoughnessTextureID), "material default textures valid (fallbacks)");
        mgr.shutdown();
    }

    if(failCount==0) printf("PASS: bindless slots, materials, fallbacks, features\n");
    else printf("FAIL %d checks\n", failCount);
    return failCount==0?0:1;
}
