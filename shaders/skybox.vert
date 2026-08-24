#version 450

// Push block: viewProjection + camera position.
layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 cameraPos; // xyz used
} pc;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 fragDir;

void main() {
    fragDir = inPosition;
    // Cube centered on camera, projected to the far plane (z = w keeps depth ~1).
    vec4 pos = pc.viewProjection * vec4(inPosition * 200.0 + pc.cameraPos.xyz, 1.0);
    gl_Position = pos.xyww;
}
