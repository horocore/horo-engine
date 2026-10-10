// HLSL 2021, Shader Model 6.0 compute. Cook through the locked target route.
// This ABI is owned by LightFrameLayout.h; bindings are explicit manifest inputs.
struct PackedLight {
    float4 positionRange;
    float3 direction;
    uint kind;
    float4 colorIntensity;
    float2 cones;
    uint2 identity;
};
struct Cluster {
    float4 planes[6];
};
StructuredBuffer<PackedLight> Lights : register(t0, space0);
StructuredBuffer<Cluster> Clusters : register(t1, space0);
RWStructuredBuffer<uint4> Membership : register(u2, space0);
RWStructuredBuffer<uint> References : register(u3, space0);
cbuffer Dispatch : register(b4, space0) {
    uint LightCount;
    uint ClusterCount;
    uint ReferencesPerCluster;
    uint Reserved;
};

bool Intersects(PackedLight light, Cluster cluster) {
    if (light.colorIntensity.w == 0.0f)
        return false;
    if (light.kind == 0)
        return true;
    if (light.positionRange.w == 0.0f)
        return false;
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex) {
        precise float distance = dot(cluster.planes[planeIndex].xyz, light.positionRange.xyz) + cluster.planes[planeIndex].w;
        if (distance < -light.positionRange.w * length(cluster.planes[planeIndex].xyz))
            return false;
    }
    return true;
}

// One thread owns one cluster's complete range. Atomic append order never affects membership.
[numthreads(64, 1, 1)]
void CullLights(uint3 dispatchThread : SV_DispatchThreadID) {
    uint clusterIndex = dispatchThread.x;
    if (Reserved != 0 || clusterIndex >= ClusterCount)
        return;
    uint offset = clusterIndex * ReferencesPerCluster;
    uint count = 0;
    uint omitted = 0;
    for (uint lightIndex = 0; lightIndex < LightCount; ++lightIndex) {
        if (!Intersects(Lights[lightIndex], Clusters[clusterIndex]))
            continue;
        if (count == ReferencesPerCluster)
            ++omitted;
        else
            References[offset + count++] = lightIndex;
    }
    Membership[clusterIndex] = uint4(offset, count, omitted, 0);
}
