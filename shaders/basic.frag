#version 450
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProjection;
} ubo;
layout(location = 0) out vec4 outColor;
void main() {
    // VP[0][0] should be ~1.72 -> white-ish if UBO readable; 0 if broken
    float v = clamp(abs(ubo.viewProjection[0][0]) / 2.0, 0.0, 1.0);
    outColor = vec4(v, v, v, 1.0);
}
