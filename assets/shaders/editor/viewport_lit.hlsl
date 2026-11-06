// Canonical development/cook source. Compile and reflect through the selected pinned Horo target route.
cbuffer ViewportParameters : register(b0, space0) {
    row_major float4x4 Mvp;
    row_major float4x4 ModelToWorld;
    row_major float4x4 ShadowViewProjection[4];
    float4 CameraAndLightCount;
    float4 LightPositionKind[16];
    float4 LightDirectionRange[16];
    float4 LightColorIntensity[16];
    float4 LightCone[16];
    float4 SelectionColorAndStrength;
    float4 ShadowSettings;
    // XYZ scales projected coordinates; XY biases are 0.5 and W is the depth bias.
    // GL: (0.5,0.5,0.5,0.5). Metal: (0.5,-0.5,1,0).
    float4 ShadowCoordinates;
    float4 CascadeSplits[4];
    float4 AtlasRects[4];
    float4 CameraForward;
    float4 ShadowBiasFilter;
    float4 BaseColor;
    float4 SceneColorPolicy;  // x1 canonical unexposed AP1, x0 explicit legacy display recipe  // Host-converted canonical linear AP1
                              // material, ingress policyV1.
};

Texture2D<float> ShadowMap : register(t1, space0);
SamplerState ShadowSampler : register(s2, space0);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal : TEXCOORD1;
};

float3 WorldNormal(float3 normal) {
    // Row-major HLSL stores the host's column-major matrix bytes without repacking, as in the qualified VFX ABI.
    float3x3 columns = (float3x3)ModelToWorld;
    float3 first = cross(columns[1], columns[2]);
    float3 second = cross(columns[2], columns[0]);
    float3 third = cross(columns[0], columns[1]);
    float determinant = dot(columns[0], first);
    float reciprocal = abs(determinant) > 0.0000001 ? 1.0 / determinant : 1.0;
    return normalize((first * normal.x + second * normal.y + third * normal.z) * reciprocal);
}

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 world = mul(float4(input.position, 1.0), ModelToWorld);
    output.position = mul(float4(input.position, 1.0), Mvp);
    output.worldPosition = world.xyz;
    output.worldNormal = WorldNormal(input.normal);
    return output;
}

float SampleCascade(uint cascade, float3 worldPosition, float3 normal, float3 lightDirection) {
    float3 localized = worldPosition - CameraAndLightCount.xyz + normal * ShadowBiasFilter.z;
    float4 clip = mul(float4(localized, 1.0), ShadowViewProjection[cascade]);
    float3 coordinate = clip.xyz / clip.w * ShadowCoordinates.xyz + float3(0.5, 0.5, ShadowCoordinates.w);
    if (any(coordinate <= 0.0) || any(coordinate >= 1.0))
        return 1.0;
    float4 rect = AtlasRects[cascade];
    float2 uv = coordinate.xy * rect.xy + rect.zw;
    float2 texel = float2(ShadowSettings.w, CameraForward.w);
    float2 minimum = rect.zw + texel * 0.5;
    float2 maximum = rect.zw + rect.xy - texel * 0.5;
    float bias = ShadowBiasFilter.x + ShadowBiasFilter.y * (1.0 - saturate(dot(normal, lightDirection)));
    int radius = (int)ShadowBiasFilter.w;
    float visibility = 0.0;
    float taps = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            if (abs(x) <= radius && abs(y) <= radius) {
                float2 sampleUv = clamp(uv + float2(x, y) * texel, minimum, maximum);
                float closest = ShadowMap.SampleLevel(ShadowSampler, sampleUv, 0.0);
                visibility += coordinate.z - bias <= closest ? 1.0 : 0.0;
                taps += 1.0;
            }
    return visibility / max(taps, 1.0);
}

float DirectionalShadow(float3 worldPosition, float3 normal, float3 lightDirection) {
    if (ShadowSettings.x == 0.0)
        return 1.0;
    float distance = dot(worldPosition - CameraAndLightCount.xyz, CameraForward.xyz);
    uint count = (uint)ShadowSettings.z;
    uint cascade = count;
    for (uint index = 0; index < count; ++index)
        if (distance <= CascadeSplits[index].x) {
            cascade = index;
            break;
        }
    if (cascade >= count)
        return 1.0;
    if (distance < CascadeSplits[0].z)
        return 1.0;
    float visibility = SampleCascade(cascade, worldPosition, normal, lightDirection);
    float blend = saturate((distance - CascadeSplits[cascade].y) / max(CascadeSplits[cascade].x - CascadeSplits[cascade].y, 0.0001));
    if (cascade + 1 < count && blend > 0.0)
        visibility = lerp(visibility, SampleCascade(cascade + 1, worldPosition, normal, lightDirection), blend);
    return visibility;
}

float4 LightDirectionAndAttenuation(uint index, float3 worldPosition, float3 normal) {
    uint kind = (uint)(LightPositionKind[index].w + 0.5);
    if (kind == 0)
        return float4(normalize(-LightDirectionRange[index].xyz), 1.0);
    float3 toLight = LightPositionKind[index].xyz - worldPosition;
    float distance = length(toLight);
    float3 direction = distance > 0.0001 ? toLight / distance : normal;
    float relativeDistance = distance / max(LightDirectionRange[index].w, 0.0001);
    float fade = max(1.0 - relativeDistance * relativeDistance, 0.0);
    float attenuation = fade * fade;
    if (kind == 2) {
        float cone = dot(normalize(LightDirectionRange[index].xyz), -direction);
        attenuation *= smoothstep(LightCone[index].y, LightCone[index].x, cone);
    }
    return float4(direction, attenuation);
}

float3 LightContribution(uint index, VertexOutput input, float3 normal, float3 viewDirection) {
    float4 light = LightDirectionAndAttenuation(index, input.worldPosition, normal);
    float diffuse = max(dot(normal, light.xyz), 0.0);
    float3 halfDirection = normalize(light.xyz + viewDirection);
    float specular = pow(max(dot(normal, halfDirection), 0.0), 48.0) * 0.18;
    float3 radiance = LightColorIntensity[index].rgb * LightColorIntensity[index].w * light.w;
    float visibility = index == (uint)ShadowSettings.y ? DirectionalShadow(input.worldPosition, normal, light.xyz) : 1.0;
    return radiance * (BaseColor.xyz * diffuse + specular) * visibility;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    float3 normal = normalize(input.worldNormal);
    float3 viewDirection = normalize(CameraAndLightCount.xyz - input.worldPosition);
    float3 lighting = BaseColor.xyz * 0.08;
    for (uint index = 0; index < min((uint)CameraAndLightCount.w, 16u); ++index)
        lighting += LightContribution(index, input, normal, viewDirection);
    if (SceneColorPolicy.x == 1.0)
        return float4(lighting, 1.0);
    float3 mapped = lighting / (lighting + 1.0);
    float3 displayColor = pow(max(mapped, 0.0), 1.0 / 2.2);
    return float4(lerp(displayColor, SelectionColorAndStrength.xyz, SelectionColorAndStrength.w), 1.0);
}
