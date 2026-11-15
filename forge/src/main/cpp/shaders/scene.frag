#version 450
// Forward-shaded demo geometry: one directional light, PCF shadow lookup and
// an anisotropically filtered albedo texture.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorld;
layout(location = 3) in vec4 vShadow;
layout(location = 4) in vec4 vColor;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 2) uniform sampler2D albedoTexture;

layout(set = 0, binding = 0) uniform SceneUbo {
    mat4 viewProj;
    mat4 lightViewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 params0;
    vec4 params1;
} ubo;

layout(location = 0) out vec4 outColor;

float shadowTap(vec2 uv, float depth, vec2 offset, float texel) {
    return texture(shadowMap, vec3(uv + offset * texel, depth));
}

float sampleShadow(vec3 worldPos, float ndl) {
    const vec3 proj = vShadow.xyz / max(vShadow.w, 1e-5);
    // The host builds a Vulkan-style [0,1] depth orthographic projection.
    const vec2 uv = proj.xy * 0.5 + 0.5;
    const float depth = proj.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        depth <= 0.0 || depth >= 1.0) {
        return 1.0;
    }

    const float bias = max(0.0008, 0.0035 * (1.0 - ndl));
    const float d = depth - bias;
    const float texel = ubo.params0.y;
    const int mode = int(ubo.params0.z + 0.5);

    if (mode <= 0) {
        return shadowTap(uv, d, vec2(0.0), texel);
    }

    float sum = 0.0;
    if (mode == 1) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                sum += shadowTap(uv, d, vec2(x, y), texel);
            }
        }
        return sum / 9.0;
    }

    // High: 5x5 PCF with a slight Poisson-like rotation to break banding.
    const vec2 taps[13] = vec2[](
        vec2( 0.0,  0.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0),
        vec2( 1.0, -1.0), vec2(-1.0, -1.0), vec2( 2.0,  0.0),
        vec2(-2.0,  0.0), vec2( 0.0,  2.0), vec2( 0.0, -2.0),
        vec2( 2.0,  1.0), vec2(-2.0, -1.0), vec2( 2.0, -1.0),
        vec2(-2.0,  1.0)
    );
    for (int i = 0; i < 13; ++i) {
        sum += shadowTap(uv, d, taps[i], texel);
    }
    return sum / 13.0;
}

void main() {
    const vec3 n = normalize(vNormal);
    const vec3 l = normalize(-ubo.lightDir.xyz);
    const float ndl = max(dot(n, l), 0.0);
    const float shadow = sampleShadow(vWorld, ndl);

    vec4 albedo = texture(albedoTexture, vUV * vColor.a);
    albedo.rgb *= vColor.rgb;

    const vec3 ambient = albedo.rgb * 0.16;
    const vec3 diffuse = albedo.rgb * (0.95 * ndl * shadow);

    // Subtle distance haze to separate the crowd layers.
    const float dist = length(vWorld.xz - ubo.cameraPos.xz);
    const float fog = 1.0 - exp(-dist * 0.0018);
    const vec3 haze = vec3(0.11, 0.14, 0.20);
    vec3 colour = mix(ambient + diffuse, haze, clamp(fog, 0.0, 0.75));

    colour *= ubo.params0.w;
    colour = colour / (colour + vec3(1.0));  // Reinhard, keeps highlights bounded
    colour = pow(colour, vec3(1.0 / 2.2));

    outColor = vec4(colour, 1.0);
}
