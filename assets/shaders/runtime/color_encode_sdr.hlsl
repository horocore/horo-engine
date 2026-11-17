// Product final SDR encoding, revision1. Input already contains display-linear UI.
// Native target must be a linear UNORM attachment: hardware sRGB encoding is forbidden.
Texture2D<float4> DisplayLinearColor : register(t0, space0);
cbuffer ColorEncodeParameters : register(b1, space0) {
    float4 OutputControl; // width,height, orderedDither0/1, quantizationLevels(255/1023)
};

float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 EncodeSrgb(float3 value) {
    const float3 low = value * 12.92;
    const float3 high = 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    return float3(value.x <= 0.0031308 ? low.x : high.x,
                  value.y <= 0.0031308 ? low.y : high.y,
                  value.z <= 0.0031308 ? low.z : high.z);
}

float OrderedDither(uint2 pixel) {
    // Fixed4x4 Bayer threshold; bounded centered quantization noise after transfer.
    static const uint threshold[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const uint index = (pixel.y & 3u) * 4u + (pixel.x & 3u);
    return ((float(threshold[index]) + 0.5) / 16.0 - 0.5) / OutputControl.w;
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const uint2 pixel = uint2(position.xy);
    if (any(pixel >= uint2(OutputControl.xy)))
        return float4(1.0, 0.0, 1.0, 1.0);
    const float4 composed = DisplayLinearColor.Load(int3(pixel, 0));
    if (!all(isfinite(composed)))
        return float4(1.0, 0.0, 1.0, 1.0);
    const float3 encoded = EncodeSrgb(saturate(composed.rgb));
    const float noise = OutputControl.z == 1.0 ? OrderedDither(pixel) : 0.0;
    return float4(saturate(encoded + noise), saturate(composed.a));
}
