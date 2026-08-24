#version 450

layout(set = 3, binding = 3) uniform samplerCube envCube;

layout(location = 0) in vec3 fragDir;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(envCube, normalize(fragDir)).rgb;
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
