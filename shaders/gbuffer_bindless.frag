#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in uint inMaterialID;

layout(location = 0) out vec4 outAlbedoAO;
layout(location = 1) out vec4 outNormalRoughness;
layout(location = 2) out vec4 outMetallicFlags;

// BINDLESS SET (Set 0)
layout(set = 0, binding = 0) uniform texture2D globalTextures[];
layout(set = 0, binding = 1) uniform sampler globalSamplers[];

struct GPUMaterial {
    uint albedoTextureID;
    uint normalTextureID;
    uint metallicRoughnessTextureID;
    uint samplerID;
    vec4 baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    vec2 padding;
};

layout(std430, set = 0, binding = 2) readonly buffer MaterialSSBO {
    GPUMaterial materials[];
};

void main() {
    GPUMaterial mat = materials[nonuniformEXT(inMaterialID)];
    
    // Sample using non-uniform dynamic indices (sampler + texture arrays)
    vec4 albedoSample = texture(sampler2D(globalTextures[nonuniformEXT(mat.albedoTextureID)], globalSamplers[nonuniformEXT(mat.samplerID)]), inUV);
    vec4 mrSample = texture(sampler2D(globalTextures[nonuniformEXT(mat.metallicRoughnessTextureID)], globalSamplers[nonuniformEXT(mat.samplerID)]), inUV);
    vec3 normalSample = texture(sampler2D(globalTextures[nonuniformEXT(mat.normalTextureID)], globalSamplers[nonuniformEXT(mat.samplerID)]), inUV).rgb;

    vec3 albedo = albedoSample.rgb * mat.baseColorFactor.rgb;
    float ao = mrSample.r;
    float roughness = mrSample.g * mat.roughnessFactor;
    float metallic = mrSample.b * mat.metallicFactor;

    outAlbedoAO = vec4(albedo, ao);
    outNormalRoughness = vec4(normalize(inWorldNormal), roughness);
    outMetallicFlags = vec4(metallic, 0.0, 0.0, 1.0);
}
