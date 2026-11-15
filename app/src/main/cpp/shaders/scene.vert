#version 450
// Shared vertex stage for the colour and shadow passes.
//
// Geometry is a unit cube submitted through vkCmdDrawIndexedIndirect. The
// per-instance model matrix and colour live in a second vertex buffer with
// input rate = 1. The scene UBO is the only per-frame descriptor.

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 3) in vec4 aModel0;
layout(location = 4) in vec4 aModel1;
layout(location = 5) in vec4 aModel2;
layout(location = 6) in vec4 aModel3;
layout(location = 7) in vec4 aColor;

layout(set = 0, binding = 0) uniform SceneUbo {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 params0;   // x=time, y=shadowTexel, z=pcfMode, w=exposure
    vec4 params1;   // x=passMode, y=instances
} ubo;

layout(push_constant) uniform Push {
    uint passMode;  // 0 = camera, 1 = light
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorld;
layout(location = 3) out vec4 vShadow;
layout(location = 4) out vec4 vColor;

void main() {
    const mat4 model = mat4(aModel0, aModel1, aModel2, aModel3);
    const vec4 world = model * vec4(aPosition, 1.0);
    const vec4 light = ubo.lightViewProj * world;

    vNormal = mat3(model) * aNormal;
    vUV = aUV;
    vWorld = world.xyz;
    vShadow = light;
    vColor = aColor;

    gl_Position = (pc.passMode == 1u) ? light : (ubo.viewProj * world);
}
