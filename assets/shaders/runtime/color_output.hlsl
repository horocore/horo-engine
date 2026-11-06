// Product fullscreen scene-to-display-linear wrapper, revision1.
// The offline cooker prefixes the byte-exact pinned processor.hlsl before this file.
// This stage excludes UI/selection and applies no output transfer encoding or dither.
Texture2D<float4> SceneColor : register(t0, space0);
StructuredBuffer<uint> ExposureWords : register(t3, space0);
cbuffer ColorOutputParameters : register(b4, space0) {
    float4 SourceSize;        // width,height, reserved0, reserved0
    float4 ExposureSelection; // GPU0/1, finite manual scale, reserved0, reserved0
    float4 ExposureIdentity;  // asfloat generationLo/Hi, producerSubmissionLo/Hi
};

float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    const float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Invalid GPU state propagates an explicit nonfinite marker. The final compositor
// displays its reserved diagnostic color; it never substitutes a successful EV.
float InvalidExposureScale() {
    return asfloat(0x7fc00000u);
}

float SelectedExposureScale() {
    if (ExposureSelection.x == 0.0)
        return ExposureSelection.y;
    const uint4 observed = uint4(ExposureWords[4], ExposureWords[5], ExposureWords[6], ExposureWords[7]);
    if (any(observed != asuint(ExposureIdentity)) || ExposureWords[3] == 3u)
        return InvalidExposureScale();
    const float scale = asfloat(ExposureWords[1]);
    return isfinite(scale) && scale > 0.0 ? scale : InvalidExposureScale();
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const uint2 pixel = uint2(position.xy);
    if (any(pixel >= uint2(SourceSize.xy)))
        return InvalidExposureScale().xxxx;
    const float4 scene = SceneColor.Load(int3(pixel, 0));
    const float scale = SelectedExposureScale();
    if (!all(isfinite(scene)) || !isfinite(scale))
        return InvalidExposureScale().xxxx;
    return HoroColorOutput(float4(scene.rgb * scale, scene.a));
}
