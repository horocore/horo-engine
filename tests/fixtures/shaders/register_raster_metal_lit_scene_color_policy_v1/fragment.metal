#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

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

struct type_ViewportParameters
{
    float4x4 Mvp;
    float4x4 ModelToWorld;
    float4x4 ShadowViewProjection[4];
    float4 CameraAndLightCount;
    float4 SelectionColorAndStrength;
    float4 ShadowSettings;
    float4 ShadowCoordinates;
    float4 CascadeSplits[4];
    float4 AtlasRects[4];
    float4 CameraForward;
    float4 ShadowBiasFilter;
    float4 ClusterDimensions;
    float4 ClusterDepth;
    float4 ViewportDimensions;
    float4 ShadowIdentity;
    float4 BaseColor;
    float4 SceneColorPolicy;
};

struct PackedLight
{
    float4 positionRange;
    packed_float3 direction;
    uint kind;
    float4 colorIntensity;
    float2 cones;
    uint identityLow;
    uint identityHigh;
};

struct type_StructuredBuffer_PackedLight
{
    PackedLight _m0[1];
};

struct LightMembership
{
    uint offset;
    uint count;
    uint omitted;
    uint reserved;
};

struct type_StructuredBuffer_LightMembership
{
    LightMembership _m0[1];
};

struct type_StructuredBuffer_uint
{
    uint _m0[1];
};

struct PSMain_out
{
    float4 out_var_SV_Target0 [[color(0)]];
};

struct PSMain_in
{
    float3 in_var_TEXCOORD0 [[user(locn0)]];
    float3 in_var_TEXCOORD1 [[user(locn1)]];
};

fragment PSMain_out PSMain(PSMain_in in [[stage_in]], constant type_ViewportParameters& ViewportParameters [[buffer(0)]], const device type_StructuredBuffer_PackedLight& ClusterLights [[buffer(3)]], const device type_StructuredBuffer_LightMembership& ClusterMembership [[buffer(4)]], const device type_StructuredBuffer_uint& ClusterReferences [[buffer(5)]], texture2d<float> ShadowMap [[texture(1)]], sampler ShadowSampler [[sampler(2)]], float4 gl_FragCoord [[position]])
{
    PSMain_out out = {};
    float4 _607;
    do
    {
        float3 _199 = fast::normalize(in.in_var_TEXCOORD1);
        float3 _203 = fast::normalize(spvFSub(ViewportParameters.CameraAndLightCount.xyz, in.in_var_TEXCOORD0));
        uint3 _207 = uint3(ViewportParameters.ClusterDimensions.xyz);
        uint2 _215 = min(uint2(spvFMul(gl_FragCoord.xy / ViewportParameters.ViewportDimensions.xy, ViewportParameters.ClusterDimensions.xy)), (_207.xy - uint2(1u)));
        float3 _37 = spvFSub(in.in_var_TEXCOORD0, ViewportParameters.CameraAndLightCount.xyz);
        float _38 = dot(_37, ViewportParameters.CameraForward.xyz);
        float _254;
        if (ViewportParameters.ClusterDepth.z > 0.5)
        {
            float _229 = precise::max(_38, ViewportParameters.ClusterDepth.x);
            float _241;
            do
            {
                float _49 = _229 / ViewportParameters.ClusterDepth.x;
                if (!(isnan(_49) || isinf(_49)))
                {
                    _241 = precise::log(_49);
                    break;
                }
                _241 = spvFSub(precise::log(_229), precise::log(ViewportParameters.ClusterDepth.x));
                break;
            } while(false);
            float _253;
            do
            {
                float _51 = ViewportParameters.ClusterDepth.y / ViewportParameters.ClusterDepth.x;
                if (!(isnan(_51) || isinf(_51)))
                {
                    _253 = precise::log(_51);
                    break;
                }
                _253 = spvFSub(precise::log(ViewportParameters.ClusterDepth.y), precise::log(ViewportParameters.ClusterDepth.x));
                break;
            } while(false);
            _254 = _241 / _253;
        }
        else
        {
            _254 = spvFSub(_38, ViewportParameters.ClusterDepth.x) / spvFSub(ViewportParameters.ClusterDepth.y, ViewportParameters.ClusterDepth.x);
        }
        uint _256 = _207.z;
        uint _48 = _215.x + (_207.x * (_215.y + (_207.y * min(uint(spvFMul(fast::clamp(_254, 0.0, 1.0), float(_256))), (_256 - 1u)))));
        uint _275 = min(ClusterMembership._m0[_48].count, uint(ViewportParameters.ViewportDimensions.z));
        float3 _277;
        _277 = ViewportParameters.BaseColor.xyz * 0.07999999821186065673828125;
        float3 _278;
        for (uint _280 = 0u; _280 < _275; _277 = _278, _280++)
        {
            uint _31 = ClusterMembership._m0[_48].offset + _280;
            if (ClusterReferences._m0[_31] < uint(ViewportParameters.ClusterDimensions.w))
            {
                float4 _340;
                do
                {
                    if (ClusterLights._m0[ClusterReferences._m0[_31]].kind == 0u)
                    {
                        _340 = float4(fast::normalize(-float3(ClusterLights._m0[ClusterReferences._m0[_31]].direction)), 1.0);
                        break;
                    }
                    float3 _64 = spvFSub(ClusterLights._m0[ClusterReferences._m0[_31]].positionRange.xyz, in.in_var_TEXCOORD0);
                    float _318 = length(_64);
                    float3 _324;
                    if (_318 > 9.9999997473787516355514526367188e-05)
                    {
                        _324 = _64 / float3(_318);
                    }
                    else
                    {
                        _324 = _199;
                    }
                    float _66 = _318 / precise::max(ClusterLights._m0[ClusterReferences._m0[_31]].positionRange.w, 9.9999997473787516355514526367188e-05);
                    float _327 = precise::max(spvFSub(1.0, spvFMul(_66, _66)), 0.0);
                    float _69 = spvFMul(_327, _327);
                    float _335;
                    if (ClusterLights._m0[ClusterReferences._m0[_31]].kind == 2u)
                    {
                        _335 = spvFMul(_69, smoothstep(ClusterLights._m0[ClusterReferences._m0[_31]].cones.y, ClusterLights._m0[ClusterReferences._m0[_31]].cones.x, dot(fast::normalize(float3(ClusterLights._m0[ClusterReferences._m0[_31]].direction)), -_324)));
                    }
                    else
                    {
                        _335 = _69;
                    }
                    _340 = float4(_324, _335);
                    break;
                } while(false);
                float _53 = dot(_199, _340.xyz);
                bool _369;
                if (ClusterLights._m0[ClusterReferences._m0[_31]].identityLow == (uint(ViewportParameters.ShadowIdentity.x) | (uint(ViewportParameters.ShadowIdentity.y) << 16u)))
                {
                    _369 = ClusterLights._m0[ClusterReferences._m0[_31]].identityHigh == (uint(ViewportParameters.ShadowIdentity.z) | (uint(ViewportParameters.ShadowIdentity.w) << 16u));
                }
                else
                {
                    _369 = false;
                }
                float _583;
                if (_369)
                {
                    float _582;
                    do
                    {
                        if (ViewportParameters.ShadowSettings.x == 0.0)
                        {
                            _582 = 1.0;
                            break;
                        }
                        uint _382 = uint(ViewportParameters.ShadowSettings.z);
                        uint _394;
                        uint _384 = 0u;
                        for (;;)
                        {
                            if (_384 < _382)
                            {
                                if (_38 <= ViewportParameters.CascadeSplits[_384].x)
                                {
                                    _394 = _384;
                                    break;
                                }
                                _384++;
                                continue;
                            }
                            else
                            {
                                _394 = _382;
                                break;
                            }
                        }
                        if (_394 >= _382)
                        {
                            _582 = 1.0;
                            break;
                        }
                        if (_38 < ViewportParameters.CascadeSplits[0].z)
                        {
                            _582 = 1.0;
                            break;
                        }
                        float4 _410;
                        float3 _418;
                        float3 _421;
                        float _490;
                        do
                        {
                            _410 = float4(spvFAdd(_37, _199 * ViewportParameters.ShadowBiasFilter.z), 1.0);
                            float4 _80 = spvFMulMatrixVector(ViewportParameters.ShadowViewProjection[_394], _410);
                            _418 = ViewportParameters.ShadowCoordinates.xyz;
                            _421 = float3(0.5, 0.5, ViewportParameters.ShadowCoordinates.w);
                            float3 _83 = spvFAdd(spvFMul(_80.xyz / float3(_80.w), _418), _421);
                            bool _429;
                            if (!any(_83 <= float3(0.0)))
                            {
                                _429 = any(_83 >= float3(1.0));
                            }
                            else
                            {
                                _429 = true;
                            }
                            if (_429)
                            {
                                _490 = 1.0;
                                break;
                            }
                            float2 _85 = spvFAdd(spvFMul(_83.xy, ViewportParameters.AtlasRects[_394].xy), ViewportParameters.AtlasRects[_394].zw);
                            float2 _441 = float2(ViewportParameters.ShadowSettings.w, ViewportParameters.CameraForward.w);
                            float2 _86 = _441 * 0.5;
                            float2 _87 = spvFAdd(ViewportParameters.AtlasRects[_394].zw, _86);
                            float2 _89 = spvFSub(spvFAdd(ViewportParameters.AtlasRects[_394].zw, ViewportParameters.AtlasRects[_394].xy), _86);
                            float _92 = spvFAdd(ViewportParameters.ShadowBiasFilter.x, spvFMul(ViewportParameters.ShadowBiasFilter.y, spvFSub(1.0, fast::clamp(_53, 0.0, 1.0))));
                            int _449 = int(ViewportParameters.ShadowBiasFilter.w);
                            float _451;
                            float _454;
                            _451 = 0.0;
                            _454 = 0.0;
                            float _452;
                            float _455;
                            for (int _456 = -2; _456 <= 2; _451 = _452, _454 = _455, _456++)
                            {
                                _452 = _451;
                                _455 = _454;
                                float _461;
                                float _463;
                                for (int _464 = -2; _464 <= 2; _452 = _461, _455 = _463, _464++)
                                {
                                    bool _474;
                                    if (abs(_464) <= _449)
                                    {
                                        _474 = abs(_456) <= _449;
                                    }
                                    else
                                    {
                                        _474 = false;
                                    }
                                    if (_474)
                                    {
                                        _461 = spvFAdd(_452, 1.0);
                                        _463 = spvFAdd(_455, float(spvFSub(_83.z, _92) <= ShadowMap.sample(ShadowSampler, fast::clamp(spvFAdd(_85, spvFMul(float2(float(_464), float(_456)), _441)), _87, _89), level(0.0)).x));
                                    }
                                    else
                                    {
                                        _461 = _452;
                                        _463 = _455;
                                    }
                                }
                            }
                            _490 = _454 / precise::max(_451, 1.0);
                            break;
                        } while(false);
                        float _496 = fast::clamp(spvFSub(_38, ViewportParameters.CascadeSplits[_394].y) / precise::max(spvFSub(ViewportParameters.CascadeSplits[_394].x, ViewportParameters.CascadeSplits[_394].y), 9.9999997473787516355514526367188e-05), 0.0, 1.0);
                        uint _77 = _394 + 1u;
                        bool _501;
                        if (_77 < _382)
                        {
                            _501 = _496 > 0.0;
                        }
                        else
                        {
                            _501 = false;
                        }
                        float _581;
                        if (_501)
                        {
                            float _579;
                            do
                            {
                                float4 _101 = spvFMulMatrixVector(ViewportParameters.ShadowViewProjection[_77], _410);
                                float3 _104 = spvFAdd(spvFMul(_101.xyz / float3(_101.w), _418), _421);
                                bool _518;
                                if (!any(_104 <= float3(0.0)))
                                {
                                    _518 = any(_104 >= float3(1.0));
                                }
                                else
                                {
                                    _518 = true;
                                }
                                if (_518)
                                {
                                    _579 = 1.0;
                                    break;
                                }
                                float2 _106 = spvFAdd(spvFMul(_104.xy, ViewportParameters.AtlasRects[_77].xy), ViewportParameters.AtlasRects[_77].zw);
                                float2 _530 = float2(ViewportParameters.ShadowSettings.w, ViewportParameters.CameraForward.w);
                                float2 _107 = _530 * 0.5;
                                float2 _108 = spvFAdd(ViewportParameters.AtlasRects[_77].zw, _107);
                                float2 _110 = spvFSub(spvFAdd(ViewportParameters.AtlasRects[_77].zw, ViewportParameters.AtlasRects[_77].xy), _107);
                                float _113 = spvFAdd(ViewportParameters.ShadowBiasFilter.x, spvFMul(ViewportParameters.ShadowBiasFilter.y, spvFSub(1.0, fast::clamp(_53, 0.0, 1.0))));
                                int _538 = int(ViewportParameters.ShadowBiasFilter.w);
                                float _540;
                                float _543;
                                _540 = 0.0;
                                _543 = 0.0;
                                float _541;
                                float _544;
                                for (int _545 = -2; _545 <= 2; _540 = _541, _543 = _544, _545++)
                                {
                                    _541 = _540;
                                    _544 = _543;
                                    float _550;
                                    float _552;
                                    for (int _553 = -2; _553 <= 2; _541 = _550, _544 = _552, _553++)
                                    {
                                        bool _563;
                                        if (abs(_553) <= _538)
                                        {
                                            _563 = abs(_545) <= _538;
                                        }
                                        else
                                        {
                                            _563 = false;
                                        }
                                        if (_563)
                                        {
                                            _550 = spvFAdd(_541, 1.0);
                                            _552 = spvFAdd(_544, float(spvFSub(_104.z, _113) <= ShadowMap.sample(ShadowSampler, fast::clamp(spvFAdd(_106, spvFMul(float2(float(_553), float(_545)), _530)), _108, _110), level(0.0)).x));
                                        }
                                        else
                                        {
                                            _550 = _541;
                                            _552 = _544;
                                        }
                                    }
                                }
                                _579 = _543 / precise::max(_540, 1.0);
                                break;
                            } while(false);
                            _581 = mix(_490, _579, _496);
                        }
                        else
                        {
                            _581 = _490;
                        }
                        _582 = _581;
                        break;
                    } while(false);
                    _583 = _582;
                }
                else
                {
                    _583 = 1.0;
                }
                _278 = spvFAdd(_277, spvFMul((ClusterLights._m0[ClusterReferences._m0[_31]].colorIntensity.xyz * ClusterLights._m0[ClusterReferences._m0[_31]].colorIntensity.w) * _340.w, spvFAdd(ViewportParameters.BaseColor.xyz * precise::max(_53, 0.0), float3(spvFMul(precise::powr(precise::max(dot(_199, fast::normalize(spvFAdd(_340.xyz, _203))), 0.0), 48.0), 0.180000007152557373046875)))) * _583);
            }
            else
            {
                _278 = _277;
            }
        }
        if (ViewportParameters.SceneColorPolicy.x == 1.0)
        {
            _607 = float4(_277, 1.0);
            break;
        }
        _607 = float4(mix(precise::powr(precise::max(_277 / spvFAdd(_277, float3(1.0)), float3(0.0)), float3(0.4545454680919647216796875)), ViewportParameters.SelectionColorAndStrength.xyz, float3(ViewportParameters.SelectionColorAndStrength.w)), 1.0);
        break;
    } while(false);
    out.out_var_SV_Target0 = _607;
    return out;
}

