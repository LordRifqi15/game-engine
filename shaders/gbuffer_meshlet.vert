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

// Upload bakes world-space vertices (model is identity) and the bindless
// material ID (padding.x), so one indirect draw covers all static materials.
layout(push_constant) uniform TransformBlock {
    mat4 viewProj;
} push;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out uint outMaterialID;

void main() {
    // Fetch vertex data directly via gl_VertexIndex resolved from CompactedIndexBuffer.
    // Upload bakes the bindless material ID into padding.x (model is identity:
    // statics are baked in world space), so one indirect draw covers all materials.
    Vertex v = globalVertices[gl_VertexIndex];

    vec4 worldPos = vec4(v.position.xyz, 1.0);
    outWorldPos = worldPos.xyz;
    outWorldNormal = v.normal.xyz;
    outUV = v.uv;
    outMaterialID = uint(v.padding.x + 0.5);

    gl_Position = push.viewProj * worldPos;
}
