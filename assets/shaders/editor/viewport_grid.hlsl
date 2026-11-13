// Canonical position-only line/grid shader; native draw state and authored colors remain host-owned.
cbuffer GridParameters : register(b0, space0) {
    row_major float4x4 Mvp;
    float4 Color;
};

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
};

struct VertexOutput {
    float4 position : SV_Position;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0), Mvp);
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    return Color;
}
