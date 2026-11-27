// Optional multiview projection with a renderer-owned ambient and directional contribution.
// The baseline multiview_mesh artifact remains unchanged.
cbuffer MultiviewEnvironmentParameters : register(b0, space0) {
    column_major float4x4 WorldToClip[32];
    column_major float4x4 LocalToWorld;
    uint ViewLayers[32];
    column_major float4x4 NormalToWorld;
    float4 EnvironmentAmbient;
    float4 EnvironmentDirectional;
    float4 EnvironmentDirection;
};

struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; };
struct VertexOutput { float4 position : SV_Position; float3 color : TEXCOORD0; };

VertexOutput VSMain(VertexInput input, uint viewIndex : SV_InstanceID) {
    VertexOutput output;
    output.position = mul(mul(float4(input.position, 1.0), LocalToWorld), WorldToClip[viewIndex]);
    if (ViewLayers[viewIndex] != 0)
        output.position = float4(2.0, 2.0, 2.0, 1.0);
    const float3 baseColor = float3(0.2, 0.6, 0.9);
    output.color = baseColor;
    if (EnvironmentAmbient.w != 0.0) {
        const float3 normal = mul(float4(input.normal, 0.0), NormalToWorld).xyz;
        const float lengthSquared = dot(normal, normal);
        const float diffuse = lengthSquared > 0.0
                                  ? max(0.0, dot(normal * rsqrt(lengthSquared), -EnvironmentDirection.xyz))
                                  : 0.0;
        output.color = baseColor * (EnvironmentAmbient.xyz + EnvironmentDirectional.xyz * diffuse);
    }
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 { return float4(input.color, 1.0); }
