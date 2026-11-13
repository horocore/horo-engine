// Canonical position-only depth pass. The host supplies its unchanged shadow-view/model product.
cbuffer ShadowParameters : register(b0, space0)
{
    row_major float4x4 Mvp;
};
struct VertexInput { [[vk::location(0)]] float3 position : POSITION; };
float4 VSMain(VertexInput input) : SV_Position
{
    return mul(float4(input.position, 1.0), Mvp);
}
void PSMain() {}
