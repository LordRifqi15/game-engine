#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outLDR;

layout(set = 0, binding = 0) uniform sampler2D hdrColor;

layout(push_constant) uniform TonemapParams {
    float exposure;
} params;

// ACES approx (Narkowicz) — stable, cheap, no LUT.
vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(hdrColor, inUV).rgb * params.exposure;
    vec3 mapped = aces(hdr);
    vec3 gamma = pow(mapped, vec3(1.0 / 2.2));
    outLDR = vec4(gamma, 1.0);
}
