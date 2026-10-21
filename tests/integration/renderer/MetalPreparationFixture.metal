#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float3 position [[attribute(0)]];
};

struct VertexOutput {
    float4 position [[position]];
    float2 uv;
};

struct Material {
    float4 tint;
};

vertex VertexOutput VertexMain(VertexInput input [[stage_in]]) {
    return {float4(input.position, 1), input.position.xy};
}

fragment float4 FragmentMain(VertexOutput input [[stage_in]], constant Material &material [[buffer(0)]],
                             texture2d<float> albedo [[texture(1)]], sampler albedoSampler [[sampler(1)]]) {
    return material.tint * albedo.sample(albedoSampler, input.uv);
}
