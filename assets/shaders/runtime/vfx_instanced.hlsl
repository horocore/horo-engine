// Cook through the existing verified shader pipeline; never compile this source at runtime.
// Logical instance buffer contains reflected ClipFromLocal and InstanceColor arrays.
#ifndef HORO_VFX_MASKED
#define HORO_VFX_MASKED 0
#endif

cbuffer VfxInstances : register(b0, space0) {
    // The locked DXC/Cross lowering turns this authored convention into the
    // canonical column-stored matrix-vector ABI consumed by the material writer.
    row_major float4x4 ClipFromLocal[128];
    float4 InstanceColor[128];
};
cbuffer VfxMaterial : register(b1, space0) {
    float4 Tint;
    float AlphaCutoff;
};
Texture2D<float4> BaseColor : register(t2, space0);
SamplerState BaseColorSampler : register(s3, space0);

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VertexOutput VSMain(VertexInput input, uint instanceIndex : SV_InstanceID) {
    VertexOutput output;
    // Keep the explicit source convention above paired with this operation;
    // final-target reflection and native qualification own the resulting ABI.
    output.position = mul(float4(input.position, 1.0), ClipFromLocal[instanceIndex]);
    output.uv = input.uv;
    output.color = InstanceColor[instanceIndex] * Tint;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    float4 color = BaseColor.Sample(BaseColorSampler, input.uv) * input.color;
#if HORO_VFX_MASKED
    clip(color.a - AlphaCutoff);
#endif
    // Scene-linear unexposed color. Native pipeline state owns the admitted
    // opaque/masked, translucent or additive blend and depth policy.
    return color;
}
