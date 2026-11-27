struct MeshInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VertexOutput VertexMain(MeshInput input) {
    VertexOutput output;
    output.position = float4(input.position, 1.0);
    output.color = float4(input.uv, input.normal.z * 0.75, 1.0);
    return output;
}

float4 FragmentMain(VertexOutput input) : SV_Target0 {
    return input.color;
}
