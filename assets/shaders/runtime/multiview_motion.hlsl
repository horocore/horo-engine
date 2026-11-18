// Separate multipass output. Color carries signed XYZ motion with a valid-sample alpha bit;
// normal fixed-function forward depth comes from the same current clip position.
cbuffer MultiviewMotionParameters : register(b0, space0) {
    column_major float4x4 CurrentLocalToClip[32];
    column_major float4x4 PreviousLocalToClip[32];
};

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
