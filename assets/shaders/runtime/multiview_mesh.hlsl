// Canonical logical parameters: buffer 1; WorldToClip 1, LocalToWorld 2, ViewLayers 3.
// Cook separate HORO_MULTIVIEW_LAYER_ROUTING=0/1 variants through the normal shader pipeline.
#ifndef HORO_MULTIVIEW_LAYER_ROUTING
#define HORO_MULTIVIEW_LAYER_ROUTING 0
#endif

cbuffer MultiviewParameters : register(b0, space0) {
    column_major float4x4 WorldToClip[32];
    column_major float4x4 LocalToWorld;
    uint ViewLayers[32];
};

struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VertexOutput {
    float4 position : SV_Position;
#if HORO_MULTIVIEW_LAYER_ROUTING
    uint layer : SV_RenderTargetArrayIndex;
#endif
};

VertexOutput VSMain(VertexInput input, uint viewIndex : SV_InstanceID) {
    VertexOutput output;
    output.position = mul(WorldToClip[viewIndex], mul(LocalToWorld, float4(input.position, 1.0)));
#if HORO_MULTIVIEW_LAYER_ROUTING
    output.layer = ViewLayers[viewIndex];
#else
    // A multipass attachment is a one-layer view; reject corrupted routing instead of selecting an adjacent eye.
    if (ViewLayers[viewIndex] != 0)
        output.position = float4(2.0, 2.0, 2.0, 1.0);
#endif
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 { return float4(0.2, 0.6, 0.9, 1.0); }
