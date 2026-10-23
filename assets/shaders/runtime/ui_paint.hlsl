// Fixed vertex/parameter ABI: UiPaintWorkload.h. Cook through the existing pinned target route.
cbuffer PaintParameters : register(b0, space0) {
    float2 Extent;
    float Opacity;
    uint Sampling;
    uint Flags;
    uint Reserved0;
    uint Reserved1;
    uint Reserved2;
    float2 MaskExtent;
    uint2 MaskReserved;
    float4 CornerRadii;
};
Texture2D<float4> PaintTexture : register(t1, space0);
Texture2D<float4> CoverageTexture : register(t2, space0);
SamplerState PaintSampler : register(s3, space0);

struct PaintInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct PaintOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float2 coverageUv : TEXCOORD1;
};

PaintOutput PaintVertex(PaintInput input) {
    PaintOutput output;
    output.position = float4(input.position.x * 2.0 / Extent.x - 1.0,
                             1.0 - input.position.y * 2.0 / Extent.y, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    output.coverageUv = input.position / Extent;
    return output;
}

float4 PaintFragment(PaintOutput input) : SV_Target0 {
    if (Reserved0 != 0u || Reserved1 != 0u || Reserved2 != 0u || any(MaskReserved != 0u))
        discard;
    float2 uv = input.uv;
    if ((Flags & 2u) != 0u)
        uv.y = 1.0 - uv.y;
    float4 sampled = PaintTexture.Sample(PaintSampler, uv);
    float coverage = 1.0;
    if ((Flags & 1u) != 0u) {
        float2 coverageUv = input.coverageUv;
        if ((Flags & 4u) != 0u)
            coverageUv.y = 1.0 - coverageUv.y;
        // Coverage has the exact target extent. Fetch its corresponding pixel independently of the source sampler.
        int2 coveragePixel = int2(clamp(coverageUv * Extent, float2(0.0, 0.0), Extent - 1.0));
        coverage = CoverageTexture.Load(int3(coveragePixel, 0)).a;
    }
    float alpha = input.color.a * Opacity * coverage;
    if (Sampling == 4u) {
        // UV remains local to the exact mask quad even under rotation/shear.
        float2 point = input.uv * MaskExtent;
        float distance = max(max(-point.x, point.x - MaskExtent.x), max(-point.y, point.y - MaskExtent.y));
        if (point.x < CornerRadii.x && point.y < CornerRadii.x)
            distance = max(distance, length(point - CornerRadii.xx) - CornerRadii.x);
        if (point.x > MaskExtent.x - CornerRadii.y && point.y < CornerRadii.y)
            distance = max(distance, length(point - float2(MaskExtent.x - CornerRadii.y, CornerRadii.y)) - CornerRadii.y);
        if (point.x > MaskExtent.x - CornerRadii.z && point.y > MaskExtent.y - CornerRadii.z)
            distance = max(distance, length(point - (MaskExtent - CornerRadii.zz)) - CornerRadii.z);
        if (point.x < CornerRadii.w && point.y > MaskExtent.y - CornerRadii.w)
            distance = max(distance, length(point - float2(CornerRadii.w, MaskExtent.y - CornerRadii.w)) - CornerRadii.w);
        float edge = max(fwidth(distance) * 0.5, 0.00001);
        alpha *= 1.0 - smoothstep(-edge, edge, distance);
        return float4(input.color.rgb * alpha, alpha);
    }
    if (Sampling == 0u)
        return float4(input.color.rgb * alpha, alpha);
    if (Sampling == 2u) {
        alpha *= sampled.a;
        return float4(input.color.rgb * alpha, alpha);
    }
    if (Sampling == 3u)
        return float4(sampled.rgb * input.color.rgb * alpha, sampled.a * alpha);
    alpha *= sampled.a;
    return float4(sampled.rgb * input.color.rgb * alpha, alpha);
}
