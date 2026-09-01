#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outHDRColor;

layout(set = 0, binding = 0) uniform sampler2D gAlbedoAO;
layout(set = 0, binding = 1) uniform sampler2D gNormalRoughness;
layout(set = 0, binding = 2) uniform sampler2D gMetallicFlags;
layout(set = 0, binding = 3) uniform sampler2D gDepth;
layout(set = 0, binding = 4) uniform sampler2D shadowMap;

layout(push_constant) uniform CameraParams {
    mat4 invViewProj;
    vec3 cameraPos;
    float _pad;
} camera;

vec3 reconstructWorldPos(vec2 uv, float depth) {
    vec4 clipSpacePos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldSpacePos = camera.invViewProj * clipSpacePos;
    return worldSpacePos.xyz / worldSpacePos.w;
}

void main() {
    float depth = texture(gDepth, inUV).r;
    if (depth >= 1.0) {
        discard;
    }
    vec3 worldPos = reconstructWorldPos(inUV, depth);
    vec4 albedoAO = texture(gAlbedoAO, inUV);
    vec4 normalRoughness = texture(gNormalRoughness, inUV);
    vec4 metallicFlags = texture(gMetallicFlags, inUV);

    vec3 N = normalize(normalRoughness.xyz);
    vec3 V = normalize(camera.cameraPos - worldPos);
    vec3 albedo = albedoAO.rgb;
    float roughness = normalRoughness.w;
    float metallic = metallicFlags.r;
    float ao = albedoAO.a;

    vec3 directLighting = vec3(0.0);
    vec3 ambient = vec3(0.03) * albedo * ao;
    outHDRColor = vec4(ambient + directLighting, 1.0);
}
