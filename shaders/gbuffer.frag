#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUV;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;

layout(location = 0) out vec4 outAlbedoAO;
layout(location = 1) out vec4 outNormalRoughness;
layout(location = 2) out vec4 outMetallicFlags;

void main() {
    vec4 albedoSample = texture(albedoMap, inUV);
    vec3 mrSample = texture(metallicRoughnessMap, inUV).rgb;

    float ao = mrSample.r;
    float roughness = mrSample.g;
    float metallic = mrSample.b;

    outAlbedoAO = vec4(albedoSample.rgb, ao);
    outNormalRoughness = vec4(normalize(inWorldNormal), roughness);
    outMetallicFlags = vec4(metallic, 0.0, 0.0, 1.0);
}
