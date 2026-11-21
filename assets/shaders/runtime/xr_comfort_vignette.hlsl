// Drawn over a loaded eye color attachment using black alpha blending.
cbuffer XrComfortParameters : register(b0, space0) {
    float4 EyeRegion; // acquired rectangle origin and reciprocal extent
    float4 Comfort;   // maximum opacity in x
};

float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const float2 uv = (position.xy - EyeRegion.xy) * EyeRegion.zw;
    const float radius = length(2.0 * uv - 1.0);
    const float opacity = saturate(Comfort.x) * smoothstep(0.35, 1.0, radius);
    return float4(0.0, 0.0, 0.0, opacity);
}
