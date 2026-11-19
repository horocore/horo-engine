// Separate multipass output. Color carries signed XYZ motion with a valid-sample alpha bit;
// normal fixed-function forward depth comes from the same current clip position.
cbuffer MultiviewMotionParameters : register(b0, space0) {
    column_major float4x4 CurrentLocalToClip[32];
    column_major float4x4 PreviousLocalToClip[32];
};

Texture2D<float> PreviousEyeDepth : register(t1, space0);

struct MotionVertex {
    float4 position : SV_Position;
    float4 currentClip : TEXCOORD0;
    float4 previousClip : TEXCOORD1;
};

MotionVertex VSMain(float3 position : POSITION, uint viewIndex : SV_InstanceID) {
    MotionVertex output;
    output.currentClip = mul(float4(position, 1.0), CurrentLocalToClip[viewIndex]);
    output.previousClip = mul(float4(position, 1.0), PreviousLocalToClip[viewIndex]);
    output.position = output.currentClip;
    return output;
}

float4 PSMain(MotionVertex input) : SV_Target0 {
    // Invalid motion still writes current depth, preserving foreground occlusion.
    if (input.currentClip.w <= 0.0 || input.previousClip.w <= 0.0)
        return 0.0;
    const float3 current = input.currentClip.xyz / input.currentClip.w;
    const float3 previous = input.previousClip.xyz / input.previousClip.w;
    if (!all(isfinite(current)) || !all(isfinite(previous)) ||
        any(abs(previous.xy) > 1.0) || previous.z < 0.0 || previous.z > 1.0)
        return 0.0;
    // The selected Vulkan recorder uses a negative-height viewport. Express Y in
    // the target image's downwards Vulkan NDC orientation.
    const float3 motion = (current - previous) * float3(1.0, -1.0, 1.0);
    return float4(motion, 1.0);
}

// Exactly representable R32Float codes, shared with EditorVulkanReprojectionCoverage.
// Zero is reserved for the attachment clear (no current fragment).
static const float ProjectionRejected = 1.0;
static const float PreviousDepthUnavailable = 2.0;
static const float Occluded = 3.0;
static const float DepthMismatch = 4.0;
static const float DepthConsistent = 5.0;

float PSCoverage(MotionVertex input) : SV_Target0 {
    if (!all(isfinite(input.currentClip)) || !all(isfinite(input.previousClip)) ||
        input.currentClip.w <= 0.0 || input.previousClip.w <= 0.0)
        return ProjectionRejected;
    const float3 previous = input.previousClip.xyz / input.previousClip.w;
    if (!all(isfinite(previous)) || any(abs(previous.xy) > 1.0) || previous.z < 0.0 || previous.z >= 1.0)
        return ProjectionRejected;

    uint width, height;
    PreviousEyeDepth.GetDimensions(width, height);
    // Reproject to the predecessor eye's texel centers in the negative-height Vulkan viewport.
    const float2 uv = previous.xy * float2(0.5, -0.5) + 0.5;
    const int2 base = int2(floor(uv * float2(width, height) - 0.5));
    if (any(base < 0) || any(base + 1 >= int2(width, height)))
        return PreviousDepthUnavailable;

    const float4 depths = float4(
        PreviousEyeDepth.Load(int3(base, 0)),
        PreviousEyeDepth.Load(int3(base + int2(1, 0), 0)),
        PreviousEyeDepth.Load(int3(base + int2(0, 1), 0)),
        PreviousEyeDepth.Load(int3(base + 1, 0)));
    if (!all(isfinite(depths)) || any(depths < 0.0) || any(depths >= 1.0))
        return PreviousDepthUnavailable;

    // Eight D32 float ULPs cover local arithmetic roundoff while rejecting resolved
    // depth discontinuities. A farther predecessor surface cannot confirm coverage.
    const uint expected = asuint(abs(previous.z));
    const float nearLimit = asfloat(expected - min(expected, 8u));
    const float farLimit = asfloat(expected + 8u);
    if (any(depths < nearLimit))
        return Occluded;
    return any(depths > farLimit) ? DepthMismatch : DepthConsistent;
}
