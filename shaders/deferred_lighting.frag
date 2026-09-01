#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outHDRColor;

// G-Buffer samplers
layout(set = 0, binding = 0) uniform sampler2D gAlbedoAO;
layout(set = 0, binding = 1) uniform sampler2D gNormalRoughness;
layout(set = 0, binding = 2) uniform sampler2D gMetallicFlags;
layout(set = 0, binding = 3) uniform sampler2D gDepth;

// Cluster SSBOs
struct GPULight { vec4 positionRadius; vec4 colorIntensity; vec4 directionAngle; uint type; uint castsShadows; uint shadowMapIndex; float padding; };
struct ClusterCell { uint offset; uint count; };

layout(std430, set = 1, binding = 0) readonly buffer LightBuffer { GPULight lights[]; };
layout(std430, set = 1, binding = 1) readonly buffer ClusterGridBuffer { ClusterCell clusters[]; };
layout(std430, set = 1, binding = 2) readonly buffer GlobalIndexList { uint globalIndexCount; uint lightIndices[]; };

layout(push_constant) uniform Params {
    mat4 invViewProj;
    mat4 view;
    vec3 cameraPos;
    uvec4 gridDim;       // x: tilesX, y: tilesY, z: slicesZ
    vec4 screenParams;   // x: screenW, y: screenH, z: zNear, w: zFar
} camera;

vec3 reconstructWorldPos(vec2 uv, float depth) {
    vec4 clipSpacePos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 worldSpacePos = camera.invViewProj * clipSpacePos;
    return worldSpacePos.xyz / worldSpacePos.w;
}

uint getClusterIndex(vec2 screenUV, float viewZ) {
    // viewZ is view-space Z (negative for in-front). Use abs and clamp to zNear
    float vz = abs(viewZ);
    float zNear = camera.screenParams.z;
    float zFar = camera.screenParams.w;
    if (vz < zNear) vz = zNear;
    if (vz > zFar) vz = zFar;
    uint tileX = uint(gl_FragCoord.x / (camera.screenParams.x / float(camera.gridDim.x)));
    uint tileY = uint(gl_FragCoord.y / (camera.screenParams.y / float(camera.gridDim.y)));
    tileX = min(tileX, camera.gridDim.x - 1);
    tileY = min(tileY, camera.gridDim.y - 1);
    float logRatio = log(vz / zNear);
    float logFarNear = log(zFar / zNear);
    uint sliceZ = uint(max(0.0, logRatio * float(camera.gridDim.z) / logFarNear));
    sliceZ = min(sliceZ, camera.gridDim.z - 1);
    return sliceZ * (camera.gridDim.x * camera.gridDim.y) + tileY * camera.gridDim.x + tileX;
}

void main() {
    float depth = texture(gDepth, inUV).r;
    if (depth >= 1.0) discard;

    vec3 worldPos = reconstructWorldPos(inUV, depth);
    float viewZ = (camera.view * vec4(worldPos, 1.0)).z;

    uint clusterIdx = getClusterIndex(inUV, viewZ);
    ClusterCell cell = clusters[clusterIdx];

    vec4 albedoAO = texture(gAlbedoAO, inUV);
    vec4 normalRoughness = texture(gNormalRoughness, inUV);
    vec4 metallicFlags = texture(gMetallicFlags, inUV);

    vec3 N = normalize(normalRoughness.xyz);
    vec3 V = normalize(camera.cameraPos - worldPos);
    vec3 albedo = albedoAO.rgb;
    float roughness = normalRoughness.w;
    float metallic = metallicFlags.r;
    float ao = albedoAO.a;

    vec3 accumulatedLighting = vec3(0.0);

    // Loop ONLY over lights in this pixel's cluster
    for (uint i = 0; i < cell.count; ++i) {
        uint lightIdx = lightIndices[cell.offset + i];
        if (lightIdx >= 4096) continue;
        GPULight light = lights[lightIdx];
        if (light.type == 0) continue; // directional handled elsewhere
        vec3 L = light.positionRadius.xyz - worldPos;
        float dist = length(L);
        float radius = light.positionRadius.w;
        if (dist > radius) continue;
        L /= dist;
        // Simple attenuation + Lambert
        float attenuation = clamp(1.0 - (dist / radius), 0.0, 1.0);
        float NdotL = max(dot(N, L), 0.0);
        vec3 color = light.colorIntensity.rgb * light.colorIntensity.w;
        accumulatedLighting += color * attenuation * NdotL;
    }

    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + accumulatedLighting * albedo;
    outHDRColor = vec4(color, 1.0);
}
