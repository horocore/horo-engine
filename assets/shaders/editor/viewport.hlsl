// Offline-only HLSL source. Canonical DXC/SPIRV-Tools compilation and admitted SPIRV-Cross reflection are required.
cbuffer ViewportParameters : register(b0, space0)
{
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
};
struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD1;
};
VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(Mvp, float4(input.position, 1.0));
    output.worldPosition = mul(ModelToWorld, float4(input.position, 1.0)).xyz;
    output.normal = mul((float3x3)NormalToWorld, input.normal);
    output.uv = input.uv;
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target0
{
    // Shared overlay geometry uses UV.x as its bounded authored batch palette index.
    // This is a real shader consumer of canonical UV, retained through the uniform-selected material mode.
    if (Tint.w < 0.0)
        return float4(OverlayColors[min((uint)input.uv.x, 2)].xyz, 1.0);
    float3 normal = normalize(input.normal);
    float3 viewDirection = normalize(CameraAndLightCount.xyz - input.worldPosition);
    const float3 baseColor = float3(0.68, 0.70, 0.74);
    float3 lighting = baseColor * 0.08;
    // PERF: The canonical scene contract admits at most 16 lights; no dynamic resource indexing is required.
    for (uint index = 0; index < (uint)CameraAndLightCount.w; ++index)
    {
        uint kind = (uint)(LightPositionKind[index].w + 0.5);
        float3 lightDirection;
        float attenuation = 1.0;
        if (kind == 0)
            lightDirection = normalize(-LightDirectionRange[index].xyz);
        else
        {
            float3 toLight = LightPositionKind[index].xyz - input.worldPosition;
            float distanceToLight = length(toLight);
            lightDirection = distanceToLight > 0.0001 ? toLight / distanceToLight : normal;
            float normalizedDistance = distanceToLight / max(LightDirectionRange[index].w, 0.0001);
            float rangeFade = max(1.0 - normalizedDistance * normalizedDistance, 0.0);
            attenuation = rangeFade * rangeFade;
            if (kind == 2)
            {
                float coneCosine = dot(normalize(LightDirectionRange[index].xyz), -lightDirection);
                attenuation *= smoothstep(LightCone[index].y, LightCone[index].x, coneCosine);
            }
        }
        float diffuse = max(dot(normal, lightDirection), 0.0);
        float3 halfDirection = normalize(lightDirection + viewDirection);
        float specular = pow(max(dot(normal, halfDirection), 0.0), 48.0) * 0.18;
        float3 radiance = LightColorIntensity[index].rgb * LightColorIntensity[index].w * attenuation;
        lighting += radiance * (baseColor * diffuse + specular);
    }
    float3 mapped = lighting / (lighting + 1.0);
    float3 displayColor = pow(max(mapped, 0.0), 1.0 / 2.2);
    return float4(lerp(displayColor, Tint.xyz, Tint.w), 1.0);
}
