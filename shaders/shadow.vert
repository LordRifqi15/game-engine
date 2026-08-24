#version 450

layout(push_constant) uniform Push {
    mat4 lightVP;
} pc;

layout(location = 0) in vec3 inPosition;
// Instance model columns occupy locations 3-6 (same binding layout as main pass).
layout(location = 1) in mat4 instanceModel;

void main() {
    gl_Position = pc.lightVP * instanceModel * vec4(inPosition, 1.0);
}
