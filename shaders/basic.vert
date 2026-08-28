#version 450

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 viewProjection;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 params;
    mat4 lightVP;
} ubo;

layout(set = 4, binding = 0) readonly buffer Joints {
    mat4 joints[];
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 9) in vec4 inJointIndices;
layout(location = 10) in vec4 inJointWeights;

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
    mat4 skin = mat4(1.0);
    float wSum = inJointWeights.x + inJointWeights.y + inJointWeights.z + inJointWeights.w;
    if (wSum > 0.001) {
        skin =
            inJointWeights.x * joints[int(inJointIndices.x)] +
            inJointWeights.y * joints[int(inJointIndices.y)] +
            inJointWeights.z * joints[int(inJointIndices.z)] +
            inJointWeights.w * joints[int(inJointIndices.w)];
    }
    mat4 modelSkin = instanceModel * skin;
    vec4 worldPos = modelSkin * vec4(inPosition, 1.0);
    gl_Position = ubo.viewProjection * worldPos;
    mat3 normalMat = mat3(modelSkin);
    fragNormal = normalize(normalMat * inNormal);
    fragColor = instanceColor.rgb;
    fragUV = inUV;
    fragParams = vec4(instanceColor.a, instanceParams.x, instanceParams.y, 1.0);
    fragWorldPos = worldPos.xyz;
    fragShadowPos = ubo.lightVP * worldPos;
}
