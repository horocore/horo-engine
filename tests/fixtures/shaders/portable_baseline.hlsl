#include "include/portable_constants.hlsli"

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VertexOutput VertexMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-0.5, -0.5),
        float2(0.0, 0.5),
        float2(0.5, -0.5),
    };
    VertexOutput output;
    output.position = float4(positions[vertexId], 0.5, 1.0);
    output.color = kTint;
    return output;
}

float4 FragmentMain(VertexOutput input) : SV_Target0 {
    return input.color;
}
