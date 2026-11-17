#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wmissing-braces"

#include <metal_stdlib>
#include <simd/simd.h>

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
[[clang::optnone]] T spvFAdd(T l, T r)
{
    return fma(T(1), l, r);
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

struct ExposureGpuState
{
    float exposureEv;
    float exposureScale;
    uint validSamples;
    uint reason;
    uint2 generation;
    uint2 sourceSubmission;
};

struct type_StructuredBuffer_ExposureGpuState
{
    ExposureGpuState _m0[1];
};

struct type_RWStructuredBuffer_ExposureGpuState
{
    ExposureGpuState _m0[1];
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

kernel void AdaptExposure(device type_RWStructuredBuffer_uint& Histogram [[buffer(1)]], device type_StructuredBuffer_ExposureGpuState& PreviousExposure [[buffer(2)]], device type_RWStructuredBuffer_ExposureGpuState& OutputExposure [[buffer(3)]], constant type_Dispatch& Dispatch [[buffer(4)]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    threadgroup spvUnsafeArray<uint, 256> Prefix;
    threadgroup spvUnsafeArray<float, 256> WeightedLog;
    threadgroup spvUnsafeArray<float, 256> Weights;
    do
    {
        bool _110;
        if (Dispatch.Controls.x == 1u)
        {
            _110 = Dispatch.Controls.y != 256u;
        }
        else
        {
            _110 = true;
        }
        if (_110)
        {
            break;
        }
        Prefix[gl_LocalInvocationIndex] = Histogram._m0[gl_LocalInvocationIndex];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint _117 = 1u; _117 < 256u; _117 *= 2u)
        {
            uint _128;
            if (gl_LocalInvocationIndex >= _117)
            {
                _128 = Prefix[gl_LocalInvocationIndex - _117];
            }
            else
            {
                _128 = 0u;
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            Prefix[gl_LocalInvocationIndex] += _128;
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
        float _132 = float(Prefix[255]);
        float _138 = float(Prefix[gl_LocalInvocationIndex]);
        Weights[gl_LocalInvocationIndex] = precise::max(0.0, spvFSub(precise::min(_138, spvFMul(_132, Dispatch.Limits.w)), precise::max(spvFSub(_138, float(Histogram._m0[gl_LocalInvocationIndex])), spvFMul(_132, Dispatch.Limits.z))));
        WeightedLog[gl_LocalInvocationIndex] = spvFMul(Weights[gl_LocalInvocationIndex], spvFAdd(Dispatch.Limits.x, spvFMul(spvFAdd(float(gl_LocalInvocationIndex), 0.5), spvFSub(Dispatch.Limits.y, Dispatch.Limits.x)) / 256.0));
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint _153 = 128u; _153 > 0u; _153 /= 2u)
        {
            if (gl_LocalInvocationIndex < _153)
            {
                uint _34 = gl_LocalInvocationIndex + _153;
                Weights[gl_LocalInvocationIndex] = spvFAdd(Weights[gl_LocalInvocationIndex], Weights[_34]);
                WeightedLog[gl_LocalInvocationIndex] = spvFAdd(WeightedLog[gl_LocalInvocationIndex], WeightedLog[_34]);
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
        if (gl_LocalInvocationIndex != 0u)
        {
            break;
        }
        float _172 = PreviousExposure._m0[0u].exposureEv;
        bool _184 = (Dispatch.Controls.z & 2147483648u) != 0u;
        bool _244;
        if (!_184)
        {
            uint _38 = PreviousExposure._m0[0u].sourceSubmission.x + 1u;
            bool _192 = _38 == 0u;
            uint _39 = PreviousExposure._m0[0u].sourceSubmission.y + uint(_192);
            bool _195 = true;
            bool _196 = false;
            bool _200;
            if (_192)
            {
                _200 = _39 == 0u;
            }
            else
            {
                _200 = false;
            }
            bool _207;
            if (!_200)
            {
                _207 = all(uint2(_38, _39) == Dispatch.Identity.zw);
            }
            else
            {
                _207 = false;
            }
            bool _217;
            if (!(isnan(_172) || isinf(_172)))
            {
                _217 = _172 >= Dispatch.Adaptation.x;
            }
            else
            {
                _217 = false;
            }
            bool _223;
            if (_217)
            {
                _223 = _172 <= Dispatch.Adaptation.y;
            }
            else
            {
                _223 = false;
            }
            bool _230;
            if (_223)
            {
                _230 = !(isnan(PreviousExposure._m0[0u].exposureScale) || isinf(PreviousExposure._m0[0u].exposureScale));
            }
            else
            {
                _230 = false;
            }
            bool _234;
            if (_230)
            {
                _234 = PreviousExposure._m0[0u].exposureScale > 0.0;
            }
            else
            {
                _234 = false;
            }
            bool _240;
            if (PreviousExposure._m0[0u].reason < 3u)
            {
                _240 = any(PreviousExposure._m0[0u].generation != uint2(0u));
            }
            else
            {
                _240 = false;
            }
            _244 = !((_207 ? _234 : false) ? _240 : false);
        }
        else
        {
            _244 = false;
        }
        if (_244)
        {
            float _255 = fast::clamp(spvFAdd(Dispatch.Timing.w, Dispatch.Timing.z), Dispatch.Adaptation.x, Dispatch.Adaptation.y);
            OutputExposure._m0[0u] = ExposureGpuState{ _255, precise::exp2(_255), 0u, 3u, Dispatch.Identity.xy, Dispatch.Identity.zw };
            break;
        }
        float _275;
        if (_184)
        {
            _275 = fast::clamp(spvFAdd(Dispatch.Timing.w, Dispatch.Timing.z), Dispatch.Adaptation.x, Dispatch.Adaptation.y);
        }
        else
        {
            _275 = _172;
        }
        bool _283;
        if (Prefix[255] != 0u)
        {
            _283 = Weights[0] <= 0.0;
        }
        else
        {
            _283 = true;
        }
        if (_283)
        {
            float _301;
            if (Dispatch.Controls.w == 1u)
            {
                _301 = _275;
            }
            else
            {
                _301 = fast::clamp(spvFAdd(Dispatch.Timing.w, Dispatch.Timing.z), Dispatch.Adaptation.x, Dispatch.Adaptation.y);
            }
            OutputExposure._m0[0u] = ExposureGpuState{ _301, precise::exp2(_301), 0u, 2u, Dispatch.Identity.xy, Dispatch.Identity.zw };
            break;
        }
        float _322 = fast::clamp(spvFAdd(spvFSub(precise::log2(Dispatch.Timing.y), WeightedLog[0] / Weights[0]), Dispatch.Timing.z), Dispatch.Adaptation.x, Dispatch.Adaptation.y);
        float _331;
        if (_322 > _275)
        {
            _331 = Dispatch.Adaptation.z;
        }
        else
        {
            _331 = Dispatch.Adaptation.w;
        }
        float _43 = spvFMul(_331, Dispatch.Timing.x);
        float _46 = spvFAdd(_275, fast::clamp(spvFSub(_322, _275), -_43, _43));
        OutputExposure._m0[0u] = ExposureGpuState{ _46, precise::exp2(_46), Prefix[255], 0u, Dispatch.Identity.xy, Dispatch.Identity.zw };
        break;
    } while(false);
}

