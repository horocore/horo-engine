// Offline-only HLSL source. Canonical DXC/SPIRV-Tools compilation and admitted SPIRV-Cross reflection are required.
cbuffer ViewportParameters : register(b0, space0) {
    float4x4 Mvp;
    float4x4 NormalToWorld;
    float4x4 ModelToWorld;
    float4 Tint;
    float4 CameraAndLightCount;
    float4 LightPositionKind[16];
    float4 LightDirectionRange[16];
    float4 LightColorIntensity[16];
    float4 LightCone[16];
    float4 OverlayColors[3];
    float4x4 ShadowViewProjection[4];
    float4 ShadowSplits[4];  // near, far, blend start, world units per texel
    float4 ShadowAtlasScaleBias[4];
    float4 ShadowSettings;      // count, selected light index, filter radius, normal bias
    float4 ShadowBiasAndTexel;  // depth bias, slope bias, inverse atlas width/height
    float4 ShadowCameraForward;
    float4 BaseColor;  // Host-converted canonical linear AP1 material, ingress policyV1.
};

Texture2D<float> DirectionalShadowAtlas : register(t1, space0);
SamplerState DirectionalShadowSampler : register(s2, space0);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD1;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(Mvp, float4(input.position, 1.0));
    output.worldPosition = mul(ModelToWorld, float4(input.position, 1.0)).xyz;
    output.normal = mul((float3x3)NormalToWorld, input.normal);
    output.uv = input.uv;
    return output;
}

float CascadeVisibility(uint cascade, float3 relativePosition, float3 normal, float3 lightDirection) {
    float3 receiver = relativePosition + normal * ShadowSettings.w;
    float4 projected = mul(ShadowViewProjection[cascade], float4(receiver, 1.0));
    float3 coordinate = projected.xyz / projected.w;
    float2 localUv = float2(coordinate.x * 0.5 + 0.5, 0.5 - coordinate.y * 0.5);
    if (any(localUv < 0.0) || any(localUv > 1.0) || coordinate.z <= 0.0 || coordinate.z >= 1.0)
        return 1.0;
    float4 rectangle = ShadowAtlasScaleBias[cascade];
    float2 texel = ShadowBiasAndTexel.zw;
    float2 uv = localUv * rectangle.xy + rectangle.zw;
    float2 minimumUv = rectangle.zw + texel * 0.5;
    float2 maximumUv = rectangle.zw + rectangle.xy - texel * 0.5;
    float bias = ShadowBiasAndTexel.x + ShadowBiasAndTexel.y * (1.0 - saturate(dot(normal, lightDirection)));
    int radius = min((int)ShadowSettings.z, 2);
    float visible = 0.0;
    float samples = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            if (abs(x) > radius || abs(y) > radius)
                continue;
            float2 sampleUv = clamp(uv + float2(x, y) * texel, minimumUv, maximumUv);
            float stored = DirectionalShadowAtlas.SampleLevel(DirectionalShadowSampler, sampleUv, 0.0);
            visible += coordinate.z - bias <= stored ? 1.0 : 0.0;
            samples += 1.0;
        }
    return visible / samples;
}

float DirectionalVisibility(float3 relativePosition, float3 normal, float3 lightDirection) {
    uint count = (uint)ShadowSettings.x;
    if (count == 0)
        return 1.0;
    float distance = dot(relativePosition, ShadowCameraForward.xyz);
    uint cascade = 0;
    while (cascade < count && distance > ShadowSplits[cascade].y)
        ++cascade;
    if (cascade == count || distance < ShadowSplits[0].x)
        return 1.0;
    float visibility = CascadeVisibility(cascade, relativePosition, normal, lightDirection);
    float blend = saturate((distance - ShadowSplits[cascade].z) / max(ShadowSplits[cascade].y - ShadowSplits[cascade].z, 0.00001));
    if (cascade + 1 < count)
        return lerp(visibility, CascadeVisibility(cascade + 1, relativePosition, normal, lightDirection), blend);
    return lerp(visibility, 1.0, blend);
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    // Shared overlay geometry uses UV.x as its bounded authored batch palette index.
    // This is a real shader consumer of canonical UV, retained through the uniform-selected material mode.
    if (Tint.w < 0.0)
        return float4(OverlayColors[min((uint)input.uv.x, 2)].xyz, 1.0);
    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(CameraAndLightCount.xyz - input.worldPosition);
    const float3 baseColor = BaseColor.xyz;
    float3 lighting = baseColor * 0.08;
    // PERF: The canonical scene contract admits at most 16 lights; no dynamic resource indexing is required.
    for (uint index = 0; index < (uint)CameraAndLightCount.w; ++index) {
        uint kind = (uint)(LightPositionKind[index].w + 0.5);
        float3 lightDirection;
        float attenuation = 1.0;
        if (kind == 0)
            lightDirection = normalize(-LightDirectionRange[index].xyz);
        else {
            float3 toLight = LightPositionKind[index].xyz - input.worldPosition;
            float distanceToLight = length(toLight);
            lightDirection = distanceToLight > 0.0001 ? toLight / distanceToLight : normal;
            float normalizedDistance = distanceToLight / max(LightDirectionRange[index].w, 0.0001);
            float rangeFade = max(1.0 - normalizedDistance * normalizedDistance, 0.0);
            attenuation = rangeFade * rangeFade;
            if (kind == 2) {
                float coneCosine = dot(normalize(LightDirectionRange[index].xyz), -lightDirection);
                attenuation *= smoothstep(LightCone[index].y, LightCone[index].x, coneCosine);
            }
        }
        float diffuse = max(dot(normal, lightDirection), 0.0);
        float3 halfDirection = normalize(lightDirection + viewDirection);
        float specular = pow(max(dot(normal, halfDirection), 0.0), 48.0) * 0.18;
        float3 radiance = LightColorIntensity[index].rgb * LightColorIntensity[index].w * attenuation;
        float visibility = index == (uint)ShadowSettings.y ? DirectionalVisibility(input.worldPosition, normal, lightDirection) : 1.0;
        lighting += radiance * (baseColor * diffuse + specular) * visibility;
    }
    float3 mapped = lighting / (lighting + 1.0);
    float3 displayColor = pow(max(mapped, 0.0), 1.0 / 2.2);
    return float4(lerp(displayColor, Tint.xyz, Tint.w), 1.0);
}
