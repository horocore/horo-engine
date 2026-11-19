// The viewport admits this compositor only for two opaque, equal-extent RGBA8 views.
cbuffer CameraCrossFadeParameters : register(b0, space0) {
    float4 DestinationWeight;
};
Texture2D<float4> SourceColor : register(t1, space0);

float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const float3 source = SourceColor.Load(int3(int2(position.xy), 0)).rgb;
    return float4(source, 1.0 - DestinationWeight.x);
}
