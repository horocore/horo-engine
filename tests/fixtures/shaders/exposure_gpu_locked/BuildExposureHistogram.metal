#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>
#include <metal_atomic>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    
    thread T& operator [] (size_t pos) thread
    {
        return elements[pos];
    }
    constexpr const thread T& operator [] (size_t pos) const thread
    {
        return elements[pos];
    }
    
    device T& operator [] (size_t pos) device
    {
        return elements[pos];
    }
    constexpr const device T& operator [] (size_t pos) const device
    {
        return elements[pos];
    }
    
    constexpr const constant T& operator [] (size_t pos) const constant
    {
        return elements[pos];
    }
    
    threadgroup T& operator [] (size_t pos) threadgroup
    {
        return elements[pos];
    }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup
    {
        return elements[pos];
    }
};

template<typename T>
[[clang::optnone]] T spvFMul(T l, T r)
{
    return fma(l, r, T(0));
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Cols> spvFMulVectorMatrix(vec<T, Rows> v, matrix<T, Cols, Rows> m)
{
    vec<T, Cols> res = vec<T, Cols>(0);
    for (uint i = Rows; i > 0; --i)
    {
        vec<T, Cols> tmp(0);
        for (uint j = 0; j < Cols; ++j)
        {
            tmp[j] = m[j][i - 1];
        }
        res = fma(tmp, vec<T, Cols>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v)
{
    vec<T, Rows> res = vec<T, Rows>(0);
    for (uint i = Cols; i > 0; --i)
    {
        res = fma(m[i - 1], vec<T, Rows>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int LCols, int LRows, int RCols, int RRows>
[[clang::optnone]] matrix<T, RCols, LRows> spvFMulMatrixMatrix(matrix<T, LCols, LRows> l, matrix<T, RCols, RRows> r)
{
    static_assert(LCols == RRows, "column-row configuration mismatch");
    matrix<T, RCols, LRows> res;
    for (uint i = 0; i < RCols; i++)
    {
        vec<T, LRows> tmp(0);
        for (uint j = 0; j < LCols; j++)
        {
            tmp = fma(vec<T, LRows>(r[i][j]), l[j], tmp);
        }
        res[i] = tmp;
    }
    return res;
}

template<typename T>
[[clang::optnone]] T spvFSub(T l, T r)
{
    return fma(T(-1), r, l);
}

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

kernel void BuildExposureHistogram(device type_RWStructuredBuffer_uint& Histogram [[buffer(1)]], constant type_Dispatch& Dispatch [[buffer(4)]], texture2d<float> SceneColor [[texture(0)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, 256> LocalHistogram;
    do
    {
        bool _81;
        if (Dispatch.Controls.x == 1u)
        {
            _81 = Dispatch.Controls.y != 256u;
        }
        else
        {
            _81 = true;
        }
        if (_81)
        {
            break;
        }
        for (uint _85 = gl_LocalInvocationIndex; _85 < 256u; )
        {
            LocalHistogram[_85] = 0u;
            _85 += 64u;
            continue;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        bool _101;
        if (gl_GlobalInvocationID.x < Dispatch.Region.z)
        {
            _101 = gl_GlobalInvocationID.y < Dispatch.Region.w;
        }
        else
        {
            _101 = false;
        }
        if (_101)
        {
            uint2 _14 = Dispatch.Region.xy + gl_GlobalInvocationID.xy;
            float4 _114 = SceneColor.read(uint2(int3(int(_14.x), int(_14.y), 0).xy), 0);
            bool4 _115 = isnan(_114);
            bool4 _116 = isinf(_114);
            if (all(not(bool4(_115.x || _116.x, _115.y || _116.y, _115.z || _116.z, _115.w || _116.w))))
            {
                uint _134 = atomic_fetch_add_explicit((threadgroup atomic_uint*)&LocalHistogram[min(255u, uint(spvFMul(fast::clamp(spvFSub(precise::log2(precise::max(dot(_114.xyz, float3(0.272228717803955078125, 0.674081742763519287109375, 0.053689517080783843994140625)), precise::exp2(Dispatch.Limits.x))), Dispatch.Limits.x) / spvFSub(Dispatch.Limits.y, Dispatch.Limits.x), 0.0, 1.0), 256.0)))], 1u, memory_order_relaxed);
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint _136 = gl_LocalInvocationIndex; _136 < 256u; )
        {
            uint _143 = atomic_fetch_add_explicit((device atomic_uint*)&Histogram._m0[_136], LocalHistogram[_136], memory_order_relaxed);
            _136 += 64u;
            continue;
        }
        break;
    } while(false);
}

