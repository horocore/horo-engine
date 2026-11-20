cbuffer WorldCanvasParameters : register(b0, space0) {
    float4x4 Mvp;
};
Texture2D<float4> CanvasColor : register(t1, space0);
SamplerState CanvasSampler : register(s2, space0);

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput projected;
    projected.position = mul(Mvp, float4(input.position, 1.0));
    projected.uv = input.uv;
    return projected;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    return CanvasColor.Sample(CanvasSampler, input.uv);
}
