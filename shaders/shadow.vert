#version 450

layout(push_constant) uniform Push {
    mat4 lightVP;
} pc;

layout(location = 0) in vec3 inPosition;
// Instance model columns occupy locations 1,2,3,4 (mat4 at location 1)
layout(location = 1) in mat4 instanceModel;
// Skinning attributes (not used for shadow depth of static meshes, but declared to match Vertex layout)
layout(location = 9) in vec4 inJointIndices;
layout(location = 10) in vec4 inJointWeights;

void main() {
    gl_Position = pc.lightVP * instanceModel * vec4(inPosition, 1.0);
}
