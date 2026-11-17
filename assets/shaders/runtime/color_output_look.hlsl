// Product fullscreen scene-to-display-linear look wrapper, revision2.
// The offline cooker prefixes the byte-exact pinned processor.hlsl before this file.
// This stage excludes UI/selection and applies no output transfer encoding or dither.
Texture2D<float4> SceneColor : register(t0, space0);
Texture2D<float4> LookCube : register(t7, space0);
StructuredBuffer<uint> ExposureWords : register(t3, space0);
cbuffer ColorOutputParameters : register(b4, space0) {
    float4 SourceSize;        // width,height, reserved0, reserved0
    float4 ExposureSelection; // GPU0/1, finite manual scale, reserved0, reserved0
    float4 ExposureIdentity;  // asfloat generationLo/Hi, producerSubmissionLo/Hi
    float4 LookDomainMinimum; // finite AP1 minimum.xyz, reserved0
    float4 LookDomainMaximum; // finite AP1 maximum.xyz, reserved0
    float4 LookControl;       // dimension2..65, interpolation0tri/1tetra, RejectOutsideV1=1, reserved0
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

// The texture stores original red-fastest RGB values; alpha is padding only.
float3 LookTexel(uint3 cell) {
    uint n = (uint)LookControl.x;
    return LookCube.Load(int3(cell.x + n * cell.y, cell.z, 0)).rgb;
}

bool LookControlsValid() {
    bool shape = LookControl.x >= 2.0 && LookControl.x <= 65.0 && LookControl.x == floor(LookControl.x);
    bool mode = LookControl.y == 0.0 || LookControl.y == 1.0;
    return shape && mode && LookControl.z == 1.0 && LookControl.w == 0.0;
}

bool LookDomainValid(float3 value) {
    return all(isfinite(value)) && all(value >= LookDomainMinimum.xyz) && all(value <= LookDomainMaximum.xyz);
}

float3 LookMix(float3 a, float3 b, float weight) {
    return a * (1.0 - weight) + b * weight;
}

float3 LookTrilinear(uint3 lo, float3 f) {
    float3 z0 = LookMix(LookMix(LookTexel(lo), LookTexel(lo + uint3(1,0,0)), f.x),
                       LookMix(LookTexel(lo + uint3(0,1,0)), LookTexel(lo + uint3(1,1,0)), f.x), f.y);
    float3 z1 = LookMix(LookMix(LookTexel(lo + uint3(0,0,1)), LookTexel(lo + uint3(1,0,1)), f.x),
                       LookMix(LookTexel(lo + uint3(0,1,1)), LookTexel(lo + uint3(1,1,1)), f.x), f.y);
    return LookMix(z0, z1, f.z);
}

// Stable descending fractions choose one of the six tetrahedra; ties retain R,G,B order.
uint3 LookAxes(float3 f) {
    uint3 order = uint3(0,1,2);
    if (f[order.x] < f[order.y]) order.xy = order.yx;
    if (f[order.y] < f[order.z]) order.yz = order.zy;
    if (f[order.x] < f[order.y]) order.xy = order.yx;
    return order;
}

float3 LookTetrahedral(uint3 lo, float3 f) {
    uint3 order = LookAxes(f);
    uint3 first = uint3(0,0,0);
    first[order.x] = 1;
    uint3 second = first;
    second[order.y] = 1;
    return LookTexel(lo) * (1.0 - f[order.x])
         + LookTexel(lo + first) * (f[order.x] - f[order.y])
         + LookTexel(lo + second) * (f[order.y] - f[order.z])
         + LookTexel(lo + uint3(1,1,1)) * f[order.z];
}

float3 ApplyCreativeLook(float3 exposed) {
    if (!LookControlsValid() || !LookDomainValid(exposed)) return InvalidExposureScale().xxx;
    float3 width = LookDomainMaximum.xyz - LookDomainMinimum.xyz;
    if (!all(isfinite(width)) || any(width <= 0.0)) return InvalidExposureScale().xxx;
    // Domain membership was checked before this numerical endpoint conditioning; outside values never reach a Load.
    float3 p = saturate((exposed - LookDomainMinimum.xyz) / width) * (LookControl.x - 1.0);
    uint3 lo = min((uint3)floor(p), uint3((uint)(LookControl.x - 2.0),(uint)(LookControl.x - 2.0),(uint)(LookControl.x - 2.0)));
    float3 f = p - float3(lo);
    return LookControl.y == 0.0 ? LookTrilinear(lo,f) : LookTetrahedral(lo,f);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    const uint2 pixel = uint2(position.xy);
    if (any(pixel >= uint2(SourceSize.xy)))
        return InvalidExposureScale().xxxx;
    const float4 scene = SceneColor.Load(int3(pixel, 0));
    const float scale = SelectedExposureScale();
    if (!all(isfinite(scene)) || !isfinite(scale))
        return InvalidExposureScale().xxxx;
    float3 looked = ApplyCreativeLook(scene.rgb * scale);
    if (!all(isfinite(looked))) return InvalidExposureScale().xxxx;
    return HoroColorOutput(float4(looked, scene.a));
}
