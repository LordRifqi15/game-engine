#version 450
#extension GL_EXT_nonuniform_qualifier : require

struct Vertex {
    vec4 position; // xyz: Pos, w: Unused
    vec4 normal;   // xyz: Normal, w: TangentSign
    vec2 uv;
    vec2 padding;
};

layout(std430, set = 0, binding = 0) readonly buffer VertexSSBO {
    Vertex globalVertices[];
};

layout(push_constant) uniform TransformBlock {
    mat4 viewProj;
    mat4 model;
    uint materialID;
} push;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out uint outMaterialID;

void main() {
    // Fetch vertex data directly via gl_VertexIndex resolved from CompactedIndexBuffer
    Vertex v = globalVertices[gl_VertexIndex];

    vec4 worldPos = push.model * vec4(v.position.xyz, 1.0);
    outWorldPos = worldPos.xyz;
    outWorldNormal = mat3(push.model) * v.normal.xyz;
    outUV = v.uv;
    outMaterialID = push.materialID;

    gl_Position = push.viewProj * worldPos;
}
