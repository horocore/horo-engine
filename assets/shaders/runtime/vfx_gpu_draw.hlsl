// Canonical direct GPU-buffer billboard draw; simulation state never round-trips through CPU instances.
ByteAddressBuffer ParticleWords : register(t0, space0);

cbuffer ViewParameters : register(b1, space0) {
    row_major float4x4 ViewProjection;
    float4 CameraRightAndWidth;
    float4 CameraUpAndHeight;
    float4 OriginDelta;
    uint4 ParticleLayout;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

uint ParticleWord(uint stream, uint slot) {
    return ParticleWords.Load((stream * ParticleLayout.y + slot) * 4u);
}

VertexOutput VfxGpuVSMain(uint vertex : SV_VertexID, uint slot : SV_InstanceID) {
    VertexOutput output;
    if (slot >= ParticleLayout.x || ParticleWord(16u, slot) == 0u) {
        output.position = float4(2, 2, 2, 1);
        output.color = float4(0, 0, 0, 0);
        return output;
    }
    float3 center = float3(asfloat(ParticleWord(0u, slot)), asfloat(ParticleWord(1u, slot)), asfloat(ParticleWord(2u, slot)));
    float2 size = float2(asfloat(ParticleWord(6u, slot)), asfloat(ParticleWord(7u, slot)));
    float rotation = asfloat(ParticleWord(8u, slot));
    float sine = sin(rotation);
    float cosine = cos(rotation);
    const float2 corners[6] = {float2(-0.5f, -0.5f), float2(0.5f, -0.5f), float2(0.5f, 0.5f),
                               float2(-0.5f, -0.5f), float2(0.5f, 0.5f),  float2(-0.5f, 0.5f)};
    float2 corner = corners[vertex] * size;
    float2 rotated = float2(corner.x * cosine - corner.y * sine, corner.x * sine + corner.y * cosine);
    float3 world = center + OriginDelta.xyz + CameraRightAndWidth.xyz * rotated.x + CameraUpAndHeight.xyz * rotated.y;
    output.position = mul(float4(world, 1), ViewProjection);
    uint color = ParticleWord(12u, slot);
    output.color =
        float4(float((color >> 24u) & 255u), float((color >> 16u) & 255u), float((color >> 8u) & 255u), float(color & 255u)) / 255.0f;
    return output;
}

float4 VfxGpuPSMain(VertexOutput input) : SV_Target0 {
    return input.color;
}
