// Offline-only HLSL source. Canonical DXC/SPIRV-Tools compilation and distinct admitted SPIRV-Cross reflection are required.
cbuffer ViewportParameters : register(b0, space0)
{
    float4x4 Mvp;
    float4x4 NormalToWorld;
    float4 Tint;
    float4 LightDirection;
    float4 LightColor;
};
struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float3 normal : NORMAL;
};
struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
    [[vk::location(1)]] float3 normal : NORMAL;
};
VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(Mvp, float4(input.position, 1.0));
    output.color = input.color;
    output.normal = mul((float3x3)NormalToWorld, input.normal);
    return output;
}
float4 PSMain(VertexOutput input) : SV_Target0
{
    // PERF: One admitted directional contribution; no dynamic resource indexing or divergent material branch.
    float3 normal = normalize(input.normal);
    float diffuse = saturate(dot(normal, -LightDirection.xyz));
    float3 color = lerp(input.color, Tint.xyz, Tint.w);
    return float4(color * (0.2 + diffuse * LightColor.xyz), 1.0);
}
