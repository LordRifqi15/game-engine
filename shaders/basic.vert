#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProjection;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 params;
    mat4 lightVP;
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 3) in mat4 instanceModel;
layout(location = 7) in vec4 instanceColor;
layout(location = 8) in vec4 instanceParams;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) out vec4 fragParams;
layout(location = 4) out vec3 fragWorldPos;
layout(location = 5) out vec4 fragShadowPos;

void main() {
    vec4 worldPos = instanceModel * vec4(inPosition, 1.0);
    gl_Position = ubo.viewProjection * worldPos;
    mat3 rot = mat3(instanceModel);
    fragNormal = normalize(rot * inNormal);
    fragColor = instanceColor.rgb;
    fragUV = inUV;
    fragParams = vec4(instanceColor.a, instanceParams.x, instanceParams.y, 1.0);
    fragWorldPos = worldPos.xyz;
    fragShadowPos = ubo.lightVP * worldPos;
}
