#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outHDRColor;

// G-Buffer samplers (set 0)
layout(set = 0, binding = 0) uniform sampler2D gAlbedoAO;
layout(set = 0, binding = 1) uniform sampler2D gNormalRoughness;
layout(set = 0, binding = 2) uniform sampler2D gMetallicFlags;
layout(set = 0, binding = 3) uniform sampler2D gDepth;
layout(set = 0, binding = 4) uniform sampler2D hizPyramid;
// Cluster SSBOs (set 1)
struct GPULight { vec4 positionRadius; vec4 colorIntensity; vec4 directionAngle; uint type; uint castsShadows; uint shadowMapIndex; float padding; };
struct ClusterCell { uint offset; uint count; };

layout(std430, set = 1, binding = 0) readonly buffer LightBuffer { GPULight lights[]; };
layout(std430, set = 1, binding = 1) readonly buffer ClusterGridBuffer { ClusterCell clusters[]; };
layout(std430, set = 1, binding = 2) readonly buffer GlobalIndexList { uint globalIndexCount; uint lightIndices[]; };

// Frame uniforms (set 2): shared camera + directional + shadow + IBL.
layout(set = 2, binding = 0) uniform FrameUBO {
    mat4 invViewProj;
    mat4 view;
    vec4 cameraPos;
    vec4 dirLightDir;    // xyz: direction TOWARD light (legacy -lightDir), w unused
    vec4 dirLightColor;  // rgb: color, w: intensity scale
    mat4 shadowVP;       // cascade 0 (matches legacy lightVP)
    vec4 shadowParams;   // x: texel size, y: bias scale, z: map size, w unused
} frame;

layout(set = 2, binding = 1) uniform sampler2D shadowMap;
layout(set = 2, binding = 2) uniform samplerCube irradianceMap;
layout(set = 2, binding = 3) uniform samplerCube prefilteredMap;
layout(set = 2, binding = 4) uniform sampler2D brdfLut;

layout(push_constant) uniform Params {
    uvec4 gridDim;       // x: tilesX, y: tilesY, z: slicesZ
    vec4 screenParams;   // x: screenW, y: screenH, z: zNear, w: zFar
    float exposure;
    int debugView;       // 0 final, 1 albedo, 2 normal, 3 depth
} params;

const float PI = 3.14159265359;

vec3 reconstructWorldPos(vec2 uv, float depth) {
    vec4 clipSpacePos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldSpacePos = frame.invViewProj * clipSpacePos;
    return worldSpacePos.xyz / worldSpacePos.w;
}

uint getClusterIndex(vec2 screenUV, float viewZ) {
    float vz = abs(viewZ);
    float zNear = params.screenParams.z;
    float zFar = params.screenParams.w;
    if (vz < zNear) vz = zNear;
    if (vz > zFar) vz = zFar;
    uint tileX = uint(gl_FragCoord.x / (params.screenParams.x / float(params.gridDim.x)));
    uint tileY = uint(gl_FragCoord.y / (params.screenParams.y / float(params.gridDim.y)));
    tileX = min(tileX, params.gridDim.x - 1);
    tileY = min(tileY, params.gridDim.y - 1);
    float logRatio = log(vz / zNear);
    float logFarNear = log(zFar / zNear);
    uint sliceZ = uint(max(0.0, logRatio * float(params.gridDim.z) / logFarNear));
    sliceZ = min(sliceZ, params.gridDim.z - 1);
    return sliceZ * (params.gridDim.x * params.gridDim.y) + tileY * params.gridDim.x + tileX;
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 0.0001);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(L, N), 0.001);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 cookTorrance(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness, vec3 lightColor) {
    vec3 H = normalize(V + L);
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (NDF * G * F) /
                    max(4.0 * max(dot(N, V), 0.001) * max(dot(L, N), 0.001), 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    float NdotL = max(dot(N, L), 0.0);
    return (kD * albedo + specular) * lightColor * NdotL;
}

float shadowFactor(vec3 worldPos, vec3 N, vec3 L) {
    vec4 shadowClip = frame.shadowVP * vec4(worldPos, 1.0);
    vec3 proj = shadowClip.xyz / shadowClip.w;
    proj = proj * 0.5 + 0.5;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z >= 1.0)
        return 1.0;
    float bias = max(frame.shadowParams.y * (1.0 - dot(N, L)), 0.0005);
    float texel = frame.shadowParams.x;
    float shadow = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            float d = texture(shadowMap, proj.xy + vec2(dx, dy) * texel).r;
            shadow += (proj.z - bias) > d ? 0.0 : 1.0;
        }
    return shadow / 9.0;
}

void main() {
    float depth = texture(gDepth, inUV).r;
    if (depth >= 1.0) discard;

    vec3 worldPos = reconstructWorldPos(inUV, depth);
    float viewZ = (frame.view * vec4(worldPos, 1.0)).z;

    vec4 albedoAO = texture(gAlbedoAO, inUV);
    vec4 normalRoughness = texture(gNormalRoughness, inUV);
    vec4 metallicFlags = texture(gMetallicFlags, inUV);

    vec3 N = normalize(normalRoughness.xyz);
    vec3 V = normalize(frame.cameraPos.xyz - worldPos);
    vec3 albedo = albedoAO.rgb;
    float roughness = clamp(normalRoughness.w, 0.05, 1.0);
    float metallic = clamp(metallicFlags.r, 0.0, 1.0);
    float ao = albedoAO.a;

    if (params.debugView == 1) { outHDRColor = vec4(albedo, 1.0); return; }
    if (params.debugView == 2) { outHDRColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (params.debugView == 3) { outHDRColor = vec4(vec3(depth), 1.0); return; }
    if (params.debugView == 5) {
        float hiz = texture(hizPyramid, inUV).r;
        outHDRColor = vec4(vec3(clamp(hiz, 0.0, 1.0)), 1.0);
        return;
    }

    // Directional light with shadow (matches legacy basic.frag model).
    vec3 L = normalize(frame.dirLightDir.xyz);
    float shadow = shadowFactor(worldPos, N, L);
    vec3 color = cookTorrance(N, V, L, albedo, metallic, roughness,
                              frame.dirLightColor.rgb * frame.dirLightColor.w)
                 * shadow * 0.6;

    // Clustered point/spot lights.
    uint clusterIdx = getClusterIndex(inUV, viewZ);
    ClusterCell cell = clusters[clusterIdx];
    for (uint i = 0; i < cell.count; ++i) {
        uint lightIdx = lightIndices[cell.offset + i];
        if (lightIdx >= 4096) continue;
        GPULight light = lights[lightIdx];
        if (light.type == 0) continue;
        vec3 toLight = light.positionRadius.xyz - worldPos;
        float dist = length(toLight);
        float radius = light.positionRadius.w;
        if (dist > radius || dist <= 0.0) continue;
        vec3 lDir = toLight / dist;
        float attenuation = clamp(1.0 - (dist / radius), 0.0, 1.0);
        vec3 lightColor = light.colorIntensity.rgb * light.colorIntensity.w;
        color += cookTorrance(N, V, lDir, albedo, metallic, roughness, lightColor) * attenuation;
    }

    // IBL ambient (matches legacy basic.frag).
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(N, V), 0.0), F0);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(prefilteredMap, R, roughness * 5.0).rgb;
    vec2 envBrdf = texture(brdfLut, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefiltered * (F * envBrdf.x + envBrdf.y);
    color += kD * irradiance * albedo * ao + specularIBL;

    outHDRColor = vec4(color * params.exposure, 1.0);
}
