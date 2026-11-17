#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct type_RWStructuredBuffer_uint
{
    uint _m0[1];
};

struct type_Dispatch
{
    uint4 Region;
    float4 Limits;
    float4 Adaptation;
    float4 Timing;
    uint4 Controls;
    uint4 Identity;
};

kernel void ClearExposureHistogram(device type_RWStructuredBuffer_uint& Histogram [[buffer(1)]], constant type_Dispatch& Dispatch [[buffer(4)]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    bool _38;
    if (Dispatch.Controls.x == 1u)
    {
        _38 = Dispatch.Controls.y == 256u;
    }
    else
    {
        _38 = false;
    }
    if (_38)
    {
        Histogram._m0[gl_LocalInvocationIndex] = 0u;
    }
}

