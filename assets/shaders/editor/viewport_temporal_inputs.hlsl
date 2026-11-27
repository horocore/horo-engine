// Single-view prepass. RasterLocalToClip includes raster jitter and the
// selected viewport orientation. Motion transforms exclude raster jitter.
cbuffer ViewportTemporalParameters : register(b0, space0) {
    column_major float4x4 RasterLocalToClip;
#if HORO_VIEWPORT_TEMPORAL_MOTION
    column_major float4x4 CurrentLocalToClip;
    column_major float4x4 PreviousLocalToClip;
    uint HasPrevious;
#else
    column_major float4x4 LocalToView;
#endif
};

struct TemporalVertex {
    float4 position : SV_Position;
#if HORO_VIEWPORT_TEMPORAL_MOTION
    float4 currentClip : TEXCOORD0;
    float4 previousClip : TEXCOORD1;
    nointerpolation uint hasPrevious : TEXCOORD2;
#else
    float viewDepth : TEXCOORD0;
#endif
};

TemporalVertex VSMain(float3 position : POSITION) {
    const float4 local = float4(position, 1.0);
    TemporalVertex output;
    output.position = mul(RasterLocalToClip, local);
#if HORO_VIEWPORT_TEMPORAL_MOTION
    output.currentClip = mul(CurrentLocalToClip, local);
    output.previousClip = mul(PreviousLocalToClip, local);
    output.hasPrevious = HasPrevious;
#else
    // The canonical view looks down -Z; its units are metres.
    output.viewDepth = -mul(LocalToView, local).z;
#endif
    return output;
}

#if HORO_VIEWPORT_TEMPORAL_MOTION
float2 PSMotion(TemporalVertex input) : SV_Target0 {
    const float2 invalid = float2(2.0, 2.0);
    if (input.hasPrevious == 0 || input.currentClip.w <= 0.0 || input.previousClip.w <= 0.0)
        return invalid;

    const float3 current = input.currentClip.xyz / input.currentClip.w;
    const float3 previous = input.previousClip.xyz / input.previousClip.w;
    if (!all(isfinite(current)) || !all(isfinite(previous)) ||
        any(abs(current.xy) > 1.0) || any(abs(previous.xy) > 1.0) || previous.z < 0.0 || previous.z > 1.0)
        return invalid;

    // The desktop renderer supplies clip transforms with downwards Y.
    // UV offsets are half the difference of the two unjittered NDC positions.
    return 0.5 * (previous.xy - current.xy);
}
#else
float PSLinearDepth(TemporalVertex input) : SV_Target0 {
    return isfinite(input.viewDepth) && input.viewDepth > 0.0 ? input.viewDepth : 0.0;
}
#endif
