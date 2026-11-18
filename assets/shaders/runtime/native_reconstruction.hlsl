// HLSL 2021, SM 6.0. NativeReconstructionKernel.h owns the dispatch ABI.
Texture2D<float4> CurrentColor : register(t0, space0);
[[vk::image_format("rgba32f")]] RWTexture2D<float4> ReconstructedColor : register(u1, space0);
Texture2D<float> CurrentDepth : register(t2, space0);
Texture2D<float2> Motion : register(t3, space0);
Texture2D<float4> PreviousColor : register(t4, space0);
Texture2D<float> PreviousDepth : register(t5, space0);
[[vk::image_format("r32f")]] RWTexture2D<float> ReconstructedDepth : register(u6, space0);

struct ReconstructionDispatch {
    uint4 Extents;           // source width/height, target width/height
    float4 SampleParameters; // current UV jitter, prior-to-current exposure scale, history weight
    float4 DepthParameters;  // absolute tolerance in meters, relative tolerance, reserved
};
[[vk::push_constant]] ConstantBuffer<ReconstructionDispatch> Dispatch;

float4 FiniteColor(float4 color) {
    return all(isfinite(color)) ? color : float4(0.0f, 0.0f, 0.0f, 0.0f);
}

int2 Texel(int2 position, uint2 extent) {
    return clamp(position, int2(0, 0), int2(extent) - 1);
}

float4 SampleCurrent(float2 uv) {
    const float2 pixel = saturate(uv) * float2(Dispatch.Extents.xy) - 0.5f;
    const int2 base = int2(floor(pixel));
    const float2 weight = frac(pixel);
    const float4 a = FiniteColor(CurrentColor.Load(int3(Texel(base, Dispatch.Extents.xy), 0)));
    const float4 b = FiniteColor(CurrentColor.Load(int3(Texel(base + int2(1, 0), Dispatch.Extents.xy), 0)));
    const float4 c = FiniteColor(CurrentColor.Load(int3(Texel(base + int2(0, 1), Dispatch.Extents.xy), 0)));
    const float4 d = FiniteColor(CurrentColor.Load(int3(Texel(base + int2(1, 1), Dispatch.Extents.xy), 0)));
    return FiniteColor(lerp(lerp(a, b, weight.x), lerp(c, d, weight.x), weight.y));
}

float4 SamplePrevious(float2 uv) {
    const float2 pixel = uv * float2(Dispatch.Extents.zw) - 0.5f;
    const int2 base = int2(floor(pixel));
    const float2 weight = frac(pixel);
    const float4 a = PreviousColor.Load(int3(Texel(base, Dispatch.Extents.zw), 0));
    const float4 b = PreviousColor.Load(int3(Texel(base + int2(1, 0), Dispatch.Extents.zw), 0));
    const float4 c = PreviousColor.Load(int3(Texel(base + int2(0, 1), Dispatch.Extents.zw), 0));
    const float4 d = PreviousColor.Load(int3(Texel(base + int2(1, 1), Dispatch.Extents.zw), 0));
    return lerp(lerp(a, b, weight.x), lerp(c, d, weight.x), weight.y);
}

bool ValidDepth(float depth) {
    return isfinite(depth) && depth > 0.0f;
}

float3 ClampHistory(float3 previous, int2 currentTexel, float3 current) {
    float3 low = current;
    float3 high = current;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float3 sample = CurrentColor.Load(int3(Texel(currentTexel + int2(x, y), Dispatch.Extents.xy), 0)).rgb;
            if (all(isfinite(sample))) {
                low = min(low, sample);
                high = max(high, sample);
            }
        }
    }
    return clamp(previous, low, high);
}

float2 TargetUv(uint2 pixel) {
    return (float2(pixel) + 0.5f) / float2(Dispatch.Extents.zw);
}

[numthreads(8, 8, 1)]
void ReconstructSpatial(uint3 thread : SV_DispatchThreadID) {
    if (all(thread.xy < Dispatch.Extents.zw))
        ReconstructedColor[thread.xy] = SampleCurrent(TargetUv(thread.xy) + Dispatch.SampleParameters.xy);
}

struct TemporalCurrentSample {
    float4 color;
    float2 targetUv;
    int2 texel;
    float depth;
};

TemporalCurrentSample SampleTemporalCurrent(uint2 pixel) {
    TemporalCurrentSample current;
    current.targetUv = TargetUv(pixel);
    const float2 sourceUv = saturate(current.targetUv + Dispatch.SampleParameters.xy);
    current.texel = Texel(int2(sourceUv * float2(Dispatch.Extents.xy)), Dispatch.Extents.xy);
    const float depth = CurrentDepth.Load(int3(current.texel, 0));
    current.depth = ValidDepth(depth) ? depth : 0.0f;
    current.color = SampleCurrent(sourceUv);
    return current;
}

[numthreads(8, 8, 1)]
void ReconstructTemporalSeed(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= Dispatch.Extents.zw))
        return;
    const TemporalCurrentSample current = SampleTemporalCurrent(thread.xy);
    ReconstructedColor[thread.xy] = current.color;
    ReconstructedDepth[thread.xy] = current.depth;
}

[numthreads(8, 8, 1)]
void ReconstructTemporal(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= Dispatch.Extents.zw))
        return;
    const TemporalCurrentSample current = SampleTemporalCurrent(thread.xy);
    float4 result = current.color;
    ReconstructedDepth[thread.xy] = current.depth;

    if (Dispatch.SampleParameters.w > 0.0f && current.depth > 0.0f) {
        const float2 motion = Motion.Load(int3(current.texel, 0));
        // History is stored on the resolved unjittered grid. Motion already excludes both jitter samples.
        const float2 previousUv = current.targetUv + motion;
        if (all(isfinite(previousUv)) && all(previousUv >= 0.0f) && all(previousUv < 1.0f)) {
            const int2 previousTexel = Texel(int2(previousUv * float2(Dispatch.Extents.zw)), Dispatch.Extents.zw);
            const float oldDepth = PreviousDepth.Load(int3(previousTexel, 0));
            const float limit = max(Dispatch.DepthParameters.x, current.depth * Dispatch.DepthParameters.y);
            if (ValidDepth(oldDepth) && abs(oldDepth - current.depth) <= limit) {
                const float3 history = SamplePrevious(previousUv).rgb * Dispatch.SampleParameters.z;
                if (all(isfinite(history)))
                    result.rgb = lerp(result.rgb, ClampHistory(history, current.texel, result.rgb), Dispatch.SampleParameters.w);
            }
        }
    }
    ReconstructedColor[thread.xy] = FiniteColor(result);
}
