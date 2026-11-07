// HLSL 2021, SM 6.0. Compile each named entry through the locked target cook.
// ExposureGpuWorkload.h owns the 32-byte state and 96-byte dispatch ABI.
// Each invocation is one closed clear -> histogram -> adaptation family.
// The native encoder must provide storage visibility between these dispatches.
struct ExposureGpuState {
    float exposureEv;
    float exposureScale;
    uint validSamples;
    uint reason;
    uint2 generation;
    uint2 sourceSubmission;
};

Texture2D<float4> SceneColor : register(t0, space0);
RWStructuredBuffer<uint> Histogram : register(u1, space0);
StructuredBuffer<ExposureGpuState> PreviousExposure : register(t2, space0);
RWStructuredBuffer<ExposureGpuState> OutputExposure : register(u3, space0);
cbuffer Dispatch : register(b4, space0) {
    uint4 Region;
    float4 Limits;      // min/max log2 luminance, lower/upper percentile
    float4 Adaptation;  // min/max EV, brighten/darken EV per second
    float4 Timing;      // selected delta seconds, neutral target, compensation EV, initial EV
    uint4 Controls;    // ABI=1, bins=256, clock bit0/native reset bit31, reset policy
    uint4 Identity;    // generation low/high, source submission low/high
};

groupshared uint LocalHistogram[256];
groupshared uint Prefix[256];
groupshared float WeightedLog[256];
groupshared float Weights[256];

// This dispatch has exactly one group. The following histogram never reads stale bins.
[numthreads(256, 1, 1)]
void ClearExposureHistogram(uint lane : SV_GroupIndex) {
    if (Controls.x == 1 && Controls.y == 256)
        Histogram[lane] = 0;
}

// ACEScg/AP1 luminance is the Y row of AP1 -> XYZ, not Rec.709 RGB weights.
// Source: Khronos Data Format Specification 1.4, section 14.7 (ACES color primaries).
uint LuminanceBin(float3 scene) {
    const float3 ap1ToY = float3(0.2722287168f, 0.6740817658f, 0.0536895174f);
    float luminance = dot(scene, ap1ToY);
    float logLuminance = log2(max(luminance, exp2(Limits.x)));
    float unit = saturate((logLuminance - Limits.x) / (Limits.y - Limits.x));
    return min(255u, (uint)(unit * 256.0f));
}

// Each 8x8 group owns a bounded source tile; integer atomics make count order irrelevant.
[numthreads(8, 8, 1)]
void BuildExposureHistogram(uint3 dispatchThread : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
    if (Controls.x != 1 || Controls.y != 256)
        return;
    for (uint bin = lane; bin < 256; bin += 64)
        LocalHistogram[bin] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (dispatchThread.x < Region.z && dispatchThread.y < Region.w) {
        float4 sample = SceneColor.Load(int3(Region.xy + dispatchThread.xy, 0));
        if (all(isfinite(sample)))
            InterlockedAdd(LocalHistogram[LuminanceBin(sample.rgb)], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint bin = lane; bin < 256; bin += 64)
        InterlockedAdd(Histogram[bin], LocalHistogram[bin]);
}

// Native owner validation establishes exact view/device/generation ownership and actual
// same-queue submission. This arithmetic check only rejects an inconsistent GPU predecessor.
bool IsPredecessor(uint2 previous, uint2 current) {
    uint low = previous.x + 1u;
    uint carry = low == 0u ? 1u : 0u;
    uint high = previous.y + carry;
    bool wrapped = carry != 0u && high == 0u;
    return !wrapped && all(uint2(low, high) == current);
}

float InitialEv() {
    return clamp(Timing.w + Timing.z, Adaptation.x, Adaptation.y);
}

// Previous storage is seeded by the host; no CPU scene pixels or histogram enter this path.
bool ValidPrevious(ExposureGpuState previous) {
    bool identity = IsPredecessor(previous.sourceSubmission, Identity.zw);
    bool value = isfinite(previous.exposureEv) && previous.exposureEv >= Adaptation.x && previous.exposureEv <= Adaptation.y;
    value = value && isfinite(previous.exposureScale) && previous.exposureScale > 0.0f;
    bool metadata = previous.reason < 3u && any(previous.generation != uint2(0u, 0u));
    return identity && value && metadata;
}

ExposureGpuState State(float ev, uint count, uint reason) {
    ExposureGpuState result;
    result.exposureEv = ev;
    result.exposureScale = exp2(ev);
    result.validSamples = count;
    result.reason = reason;
    result.generation = Identity.xy;
    result.sourceSubmission = Identity.zw;
    return result;
}

float Adapt(float previous, float target) {
    float rate = target > previous ? Adaptation.z : Adaptation.w;
    float maximumStep = rate * Timing.x;
    return previous + clamp(target - previous, -maximumStep, maximumStep);
}

// Prefix scan and tree reduction use a fixed 256 lanes, independent of source extent.
void PrefixHistogram(uint lane) {
    Prefix[lane] = Histogram[lane];
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < 256; offset *= 2) {
        uint addend = lane >= offset ? Prefix[lane - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        Prefix[lane] += addend;
        GroupMemoryBarrierWithGroupSync();
    }
}

void PercentileWeight(uint lane) {
    float total = (float)Prefix[255];
    float lower = total * Limits.z;
    float upper = total * Limits.w;
    float end = (float)Prefix[lane];
    float start = end - (float)Histogram[lane];
    Weights[lane] = max(0.0f, min(end, upper) - max(start, lower));
    float center = Limits.x + ((float)lane + 0.5f) * (Limits.y - Limits.x) / 256.0f;
    WeightedLog[lane] = Weights[lane] * center;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 128; offset > 0; offset /= 2) {
        if (lane < offset) {
            Weights[lane] += Weights[lane + offset];
            WeightedLog[lane] += WeightedLog[lane + offset];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(256, 1, 1)]
void AdaptExposure(uint lane : SV_GroupIndex) {
    if (Controls.x != 1 || Controls.y != 256)
        return;
    PrefixHistogram(lane);
    PercentileWeight(lane);
    if (lane != 0)
        return;
    ExposureGpuState predecessor = PreviousExposure[0];
    bool reset = (Controls.z & 0x80000000u) != 0u;
    if (!reset && !ValidPrevious(predecessor)) {
        // Invalid=3 remains observable in device state and propagates until an explicit reset.
        OutputExposure[0] = State(InitialEv(), 0, 3);
        return;
    }
    float previous = reset ? InitialEv() : predecessor.exposureEv;
    uint count = Prefix[255];
    if (count == 0 || Weights[0] <= 0.0f) {
        // ExposureResetPolicy::InitialValue=0, RetainLastValid=1; Empty=2.
        float emptyEv = Controls.w == 1 ? previous : InitialEv();
        OutputExposure[0] = State(emptyEv, 0, 2);
        return;
    }
    float logAverage = WeightedLog[0] / Weights[0];
    float target = clamp(log2(Timing.y) - logAverage + Timing.z, Adaptation.x, Adaptation.y);
    OutputExposure[0] = State(Adapt(previous, target), count, 0);
}
