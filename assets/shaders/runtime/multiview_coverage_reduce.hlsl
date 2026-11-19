// The host submits the exact two active rectangles and resets this frame's counts.
Texture2D<float> CoverageLeft : register(t0, space0);
Texture2D<float> CoverageRight : register(t1, space0);
RWStructuredBuffer<uint2> CoverageCounts : register(u2, space0);

cbuffer CoverageRectangles : register(b3, space0) {
    uint4 LeftRect;
    uint4 RightRect;
};

static const uint CoverageTileExtent = 8;
static const uint CoverageTileSamples = CoverageTileExtent * CoverageTileExtent;
static const float DepthConsistent = 5.0;
groupshared uint2 groupCounts[CoverageTileSamples];

[numthreads(CoverageTileExtent, CoverageTileExtent, 1)]
void CSReduceCoverage(uint3 pixel : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
    // A dispatch Z slice is one dense eye index. It is uniform within the workgroup.
    if (pixel.z > 1)
        return;
    const uint4 rect = pixel.z == 0 ? LeftRect : RightRect;
    uint2 counts = 0;
    if (all(pixel.xy < rect.zw)) {
        const int3 location = int3(rect.xy + pixel.xy, 0);
        const float code = pixel.z == 0 ? CoverageLeft.Load(location) : CoverageRight.Load(location);
        // Only the separately qualified depth-consistent code can resolve a sample.
        counts = uint2(1, code == DepthConsistent ? 0 : 1);
    }

    groupCounts[lane] = counts;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = CoverageTileSamples / 2; stride != 0; stride >>= 1) {
        if (lane < stride)
            groupCounts[lane] += groupCounts[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0 && groupCounts[0].x != 0) {
        InterlockedAdd(CoverageCounts[pixel.z].x, groupCounts[0].x);
        if (groupCounts[0].y != 0)
            InterlockedAdd(CoverageCounts[pixel.z].y, groupCounts[0].y);
    }
}
