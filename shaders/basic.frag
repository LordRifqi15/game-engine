#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProjection;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 params;
    mat4 lightVP;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D materialTex;
layout(set = 2, binding = 0) uniform sampler2D shadowMap;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec4 fragParams;
layout(location = 4) in vec3 fragWorldPos;
layout(location = 5) in vec4 fragShadowPos;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

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

float shadowFactor(vec3 N, vec3 L) {
    vec3 proj = fragShadowPos.xyz / fragShadowPos.w;
    proj = proj * 0.5 + 0.5;

    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z >= 1.0)
        return 1.0;

    float bias = max(0.005 * (1.0 - dot(N, L)), 0.0005);
    float texel = 1.0 / 2048.0;
    float shadow = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            float d = texture(shadowMap, proj.xy + vec2(dx, dy) * texel).r;
            shadow += (proj.z - bias) > d ? 0.0 : 1.0;
        }
    return shadow / 9.0;
}

void main() {
    vec3 albedo = fragColor;
    if (fragParams.x > 0.5) albedo *= texture(materialTex, fragUV).rgb;
    float metallic = clamp(fragParams.y, 0.0, 1.0);
    float roughness = clamp(fragParams.z, 0.05, 1.0);

    vec3 N = normalize(fragNormal);
    vec3 V = normalize(ubo.cameraPos.xyz - fragWorldPos);
    vec3 L = normalize(-ubo.lightDir.xyz);
    vec3 H = normalize(V + L);

    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (NDF * G * F) /
                    max(4.0 * max(dot(N, V), 0.001) * max(dot(L, N), 0.001), 0.001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    float shadow = shadowFactor(N, L);

    // Direct-light intensity folded in (no 1/PI split).
    vec3 Lo = (kD * albedo + specular) * ubo.lightColor.rgb * NdotL * shadow * 0.6;
    vec3 ambient = ubo.params.x * albedo;
    vec3 color = ambient + Lo;

    color = pow(clamp(color, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}
