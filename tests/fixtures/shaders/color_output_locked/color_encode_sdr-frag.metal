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
[[clang::optnone]] T spvFAdd(T l, T r)
{
    return fma(T(1), l, r);
}

template<typename T>
[[clang::optnone]] T spvFSub(T l, T r)
{
    return fma(T(-1), r, l);
}

struct type_ColorEncodeParameters
{
    float4 OutputControl;
};

constant spvUnsafeArray<uint, 16> _77 = spvUnsafeArray<uint, 16>({ 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u });

struct PSMain_out
{
    float4 out_var_SV_Target0 [[color(0)]];
};

fragment PSMain_out PSMain(constant type_ColorEncodeParameters& ColorEncodeParameters [[buffer(1)]], texture2d<float> DisplayLinearColor [[texture(0)]], float4 gl_FragCoord [[position]])
{
    PSMain_out out = {};
    float4 _158;
    do
    {
        uint2 _83 = uint2(gl_FragCoord.xy);
        if (any(_83 >= uint2(ColorEncodeParameters.OutputControl.xy)))
        {
            _158 = float4(1.0, 0.0, 1.0, 1.0);
            break;
        }
        uint _92 = _83.x;
        uint _93 = _83.y;
        float4 _99 = DisplayLinearColor.read(uint2(int3(int(_92), int(_93), 0).xy), 0);
        bool4 _100 = isnan(_99);
        bool4 _101 = isinf(_99);
        if (!all(not(bool4(_100.x || _101.x, _100.y || _101.y, _100.z || _101.z, _100.w || _101.w))))
        {
            _158 = float4(1.0, 0.0, 1.0, 1.0);
            break;
        }
        float3 _109 = fast::clamp(_99.xyz, float3(0.0), float3(1.0));
        float3 _11 = _109 * 12.9200000762939453125;
        float3 _13 = spvFSub(precise::powr(_109, float3(0.4166666567325592041015625)) * 1.05499994754791259765625, float3(0.054999999701976776123046875));
        float _118;
        if (_109.x <= 0.003130800090730190277099609375)
        {
            _118 = _11.x;
        }
        else
        {
            _118 = _13.x;
        }
        float _126;
        if (_109.y <= 0.003130800090730190277099609375)
        {
            _126 = _11.y;
        }
        else
        {
            _126 = _13.y;
        }
        float _134;
        if (_109.z <= 0.003130800090730190277099609375)
        {
            _134 = _11.z;
        }
        else
        {
            _134 = _13.z;
        }
        float _149;
        if (ColorEncodeParameters.OutputControl.z == 1.0)
        {
            _149 = spvFSub(spvFAdd(float(_77[((_93 & 3u) * 4u) + (_92 & 3u)]), 0.5) / 16.0, 0.5) / ColorEncodeParameters.OutputControl.w;
        }
        else
        {
            _149 = 0.0;
        }
        _158 = float4(fast::clamp(spvFAdd(float3(_118, _126, _134), float3(_149)), float3(0.0), float3(1.0)), fast::clamp(_99.w, 0.0, 1.0));
        break;
    } while(false);
    out.out_var_SV_Target0 = _158;
    return out;
}

