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
    float3 _196 = fast::normalize(in.in_var_TEXCOORD1);
    float3 _200 = fast::normalize(spvFSub(ViewportParameters.CameraAndLightCount.xyz, in.in_var_TEXCOORD0));
    uint3 _204 = uint3(ViewportParameters.ClusterDimensions.xyz);
    uint2 _212 = min(uint2(spvFMul(gl_FragCoord.xy / ViewportParameters.ViewportDimensions.xy, ViewportParameters.ClusterDimensions.xy)), (_204.xy - uint2(1u)));
    float3 _37 = spvFSub(in.in_var_TEXCOORD0, ViewportParameters.CameraAndLightCount.xyz);
    float _38 = dot(_37, ViewportParameters.CameraForward.xyz);
    float _251;
    if (ViewportParameters.ClusterDepth.z > 0.5)
    {
        float _226 = precise::max(_38, ViewportParameters.ClusterDepth.x);
        float _238;
        do
        {
            float _49 = _226 / ViewportParameters.ClusterDepth.x;
            if (!(isnan(_49) || isinf(_49)))
            {
                _238 = precise::log(_49);
                break;
            }
            _238 = spvFSub(precise::log(_226), precise::log(ViewportParameters.ClusterDepth.x));
            break;
        } while(false);
        float _250;
        do
        {
            float _51 = ViewportParameters.ClusterDepth.y / ViewportParameters.ClusterDepth.x;
            if (!(isnan(_51) || isinf(_51)))
            {
                _250 = precise::log(_51);
                break;
            }
            _250 = spvFSub(precise::log(ViewportParameters.ClusterDepth.y), precise::log(ViewportParameters.ClusterDepth.x));
            break;
        } while(false);
        _251 = _238 / _250;
    }
    else
    {
        _251 = spvFSub(_38, ViewportParameters.ClusterDepth.x) / spvFSub(ViewportParameters.ClusterDepth.y, ViewportParameters.ClusterDepth.x);
    }
    uint _253 = _204.z;
    uint _48 = _212.x + (_204.x * (_212.y + (_204.y * min(uint(spvFMul(fast::clamp(_251, 0.0, 1.0), float(_253))), (_253 - 1u)))));
    uint _272 = min(ClusterMembership._m0[_48].count, uint(ViewportParameters.ViewportDimensions.z));
    float3 _274;
    _274 = ViewportParameters.BaseColor.xyz * 0.07999999821186065673828125;
    float3 _275;
    for (uint _277 = 0u; _277 < _272; _274 = _275, _277++)
    {
        uint _31 = ClusterMembership._m0[_48].offset + _277;
        if (ClusterReferences._m0[_31] < uint(ViewportParameters.ClusterDimensions.w))
        {
            float4 _337;
            do
            {
                if (ClusterLights._m0[ClusterReferences._m0[_31]].kind == 0u)
                {
                    _337 = float4(fast::normalize(-float3(ClusterLights._m0[ClusterReferences._m0[_31]].direction)), 1.0);
                    break;
                }
                float3 _64 = spvFSub(ClusterLights._m0[ClusterReferences._m0[_31]].positionRange.xyz, in.in_var_TEXCOORD0);
                float _315 = length(_64);
                float3 _321;
                if (_315 > 9.9999997473787516355514526367188e-05)
                {
                    _321 = _64 / float3(_315);
                }
                else
                {
                    _321 = _196;
                }
                float _66 = _315 / precise::max(ClusterLights._m0[ClusterReferences._m0[_31]].positionRange.w, 9.9999997473787516355514526367188e-05);
                float _324 = precise::max(spvFSub(1.0, spvFMul(_66, _66)), 0.0);
                float _69 = spvFMul(_324, _324);
                float _332;
                if (ClusterLights._m0[ClusterReferences._m0[_31]].kind == 2u)
                {
                    _332 = spvFMul(_69, smoothstep(ClusterLights._m0[ClusterReferences._m0[_31]].cones.y, ClusterLights._m0[ClusterReferences._m0[_31]].cones.x, dot(fast::normalize(float3(ClusterLights._m0[ClusterReferences._m0[_31]].direction)), -_321)));
                }
                else
                {
                    _332 = _69;
                }
                _337 = float4(_321, _332);
                break;
            } while(false);
            float _53 = dot(_196, _337.xyz);
            bool _366;
            if (ClusterLights._m0[ClusterReferences._m0[_31]].identityLow == (uint(ViewportParameters.ShadowIdentity.x) | (uint(ViewportParameters.ShadowIdentity.y) << 16u)))
            {
                _366 = ClusterLights._m0[ClusterReferences._m0[_31]].identityHigh == (uint(ViewportParameters.ShadowIdentity.z) | (uint(ViewportParameters.ShadowIdentity.w) << 16u));
            }
            else
            {
                _366 = false;
            }
            float _580;
            if (_366)
            {
                float _579;
                do
                {
                    if (ViewportParameters.ShadowSettings.x == 0.0)
                    {
                        _579 = 1.0;
                        break;
                    }
                    uint _379 = uint(ViewportParameters.ShadowSettings.z);
                    uint _391;
                    uint _381 = 0u;
                    for (;;)
                    {
                        if (_381 < _379)
                        {
                            if (_38 <= ViewportParameters.CascadeSplits[_381].x)
                            {
                                _391 = _381;
                                break;
                            }
                            _381++;
                            continue;
                        }
                        else
                        {
                            _391 = _379;
                            break;
                        }
                    }
                    if (_391 >= _379)
                    {
                        _579 = 1.0;
                        break;
                    }
                    if (_38 < ViewportParameters.CascadeSplits[0].z)
                    {
                        _579 = 1.0;
                        break;
                    }
                    float4 _407;
                    float3 _415;
                    float3 _418;
                    float _487;
                    do
                    {
                        _407 = float4(spvFAdd(_37, _196 * ViewportParameters.ShadowBiasFilter.z), 1.0);
                        float4 _80 = spvFMulMatrixVector(ViewportParameters.ShadowViewProjection[_391], _407);
                        _415 = ViewportParameters.ShadowCoordinates.xyz;
                        _418 = float3(0.5, 0.5, ViewportParameters.ShadowCoordinates.w);
                        float3 _83 = spvFAdd(spvFMul(_80.xyz / float3(_80.w), _415), _418);
                        bool _426;
                        if (!any(_83 <= float3(0.0)))
                        {
                            _426 = any(_83 >= float3(1.0));
                        }
                        else
                        {
                            _426 = true;
                        }
                        if (_426)
                        {
                            _487 = 1.0;
                            break;
                        }
                        float2 _85 = spvFAdd(spvFMul(_83.xy, ViewportParameters.AtlasRects[_391].xy), ViewportParameters.AtlasRects[_391].zw);
                        float2 _438 = float2(ViewportParameters.ShadowSettings.w, ViewportParameters.CameraForward.w);
                        float2 _86 = _438 * 0.5;
                        float2 _87 = spvFAdd(ViewportParameters.AtlasRects[_391].zw, _86);
                        float2 _89 = spvFSub(spvFAdd(ViewportParameters.AtlasRects[_391].zw, ViewportParameters.AtlasRects[_391].xy), _86);
                        float _92 = spvFAdd(ViewportParameters.ShadowBiasFilter.x, spvFMul(ViewportParameters.ShadowBiasFilter.y, spvFSub(1.0, fast::clamp(_53, 0.0, 1.0))));
                        int _446 = int(ViewportParameters.ShadowBiasFilter.w);
                        float _448;
                        float _451;
                        _448 = 0.0;
                        _451 = 0.0;
                        float _449;
                        float _452;
                        for (int _453 = -2; _453 <= 2; _448 = _449, _451 = _452, _453++)
                        {
                            _449 = _448;
                            _452 = _451;
                            float _458;
                            float _460;
                            for (int _461 = -2; _461 <= 2; _449 = _458, _452 = _460, _461++)
                            {
                                bool _471;
                                if (abs(_461) <= _446)
                                {
                                    _471 = abs(_453) <= _446;
                                }
                                else
                                {
                                    _471 = false;
                                }
                                if (_471)
                                {
                                    _458 = spvFAdd(_449, 1.0);
                                    _460 = spvFAdd(_452, float(spvFSub(_83.z, _92) <= ShadowMap.sample(ShadowSampler, fast::clamp(spvFAdd(_85, spvFMul(float2(float(_461), float(_453)), _438)), _87, _89), level(0.0)).x));
                                }
                                else
                                {
                                    _458 = _449;
                                    _460 = _452;
                                }
                            }
                        }
                        _487 = _451 / precise::max(_448, 1.0);
                        break;
                    } while(false);
                    float _493 = fast::clamp(spvFSub(_38, ViewportParameters.CascadeSplits[_391].y) / precise::max(spvFSub(ViewportParameters.CascadeSplits[_391].x, ViewportParameters.CascadeSplits[_391].y), 9.9999997473787516355514526367188e-05), 0.0, 1.0);
                    uint _77 = _391 + 1u;
                    bool _498;
                    if (_77 < _379)
                    {
                        _498 = _493 > 0.0;
                    }
                    else
                    {
                        _498 = false;
                    }
                    float _578;
                    if (_498)
                    {
                        float _576;
                        do
                        {
                            float4 _101 = spvFMulMatrixVector(ViewportParameters.ShadowViewProjection[_77], _407);
                            float3 _104 = spvFAdd(spvFMul(_101.xyz / float3(_101.w), _415), _418);
                            bool _515;
                            if (!any(_104 <= float3(0.0)))
                            {
                                _515 = any(_104 >= float3(1.0));
                            }
                            else
                            {
                                _515 = true;
                            }
                            if (_515)
                            {
                                _576 = 1.0;
                                break;
                            }
                            float2 _106 = spvFAdd(spvFMul(_104.xy, ViewportParameters.AtlasRects[_77].xy), ViewportParameters.AtlasRects[_77].zw);
                            float2 _527 = float2(ViewportParameters.ShadowSettings.w, ViewportParameters.CameraForward.w);
                            float2 _107 = _527 * 0.5;
                            float2 _108 = spvFAdd(ViewportParameters.AtlasRects[_77].zw, _107);
                            float2 _110 = spvFSub(spvFAdd(ViewportParameters.AtlasRects[_77].zw, ViewportParameters.AtlasRects[_77].xy), _107);
                            float _113 = spvFAdd(ViewportParameters.ShadowBiasFilter.x, spvFMul(ViewportParameters.ShadowBiasFilter.y, spvFSub(1.0, fast::clamp(_53, 0.0, 1.0))));
                            int _535 = int(ViewportParameters.ShadowBiasFilter.w);
                            float _537;
                            float _540;
                            _537 = 0.0;
                            _540 = 0.0;
                            float _538;
                            float _541;
                            for (int _542 = -2; _542 <= 2; _537 = _538, _540 = _541, _542++)
                            {
                                _538 = _537;
                                _541 = _540;
                                float _547;
                                float _549;
                                for (int _550 = -2; _550 <= 2; _538 = _547, _541 = _549, _550++)
                                {
                                    bool _560;
                                    if (abs(_550) <= _535)
                                    {
                                        _560 = abs(_542) <= _535;
                                    }
                                    else
                                    {
                                        _560 = false;
                                    }
                                    if (_560)
                                    {
                                        _547 = spvFAdd(_538, 1.0);
                                        _549 = spvFAdd(_541, float(spvFSub(_104.z, _113) <= ShadowMap.sample(ShadowSampler, fast::clamp(spvFAdd(_106, spvFMul(float2(float(_550), float(_542)), _527)), _108, _110), level(0.0)).x));
                                    }
                                    else
                                    {
                                        _547 = _538;
                                        _549 = _541;
                                    }
                                }
                            }
                            _576 = _540 / precise::max(_537, 1.0);
                            break;
                        } while(false);
                        _578 = mix(_487, _576, _493);
                    }
                    else
                    {
                        _578 = _487;
                    }
                    _579 = _578;
                    break;
                } while(false);
                _580 = _579;
            }
            else
            {
                _580 = 1.0;
            }
            _275 = spvFAdd(_274, spvFMul((ClusterLights._m0[ClusterReferences._m0[_31]].colorIntensity.xyz * ClusterLights._m0[ClusterReferences._m0[_31]].colorIntensity.w) * _337.w, spvFAdd(ViewportParameters.BaseColor.xyz * precise::max(_53, 0.0), float3(spvFMul(precise::powr(precise::max(dot(_196, fast::normalize(spvFAdd(_337.xyz, _200))), 0.0), 48.0), 0.180000007152557373046875)))) * _580);
        }
        else
        {
            _275 = _274;
        }
    }
    out.out_var_SV_Target0 = float4(mix(precise::powr(precise::max(_274 / spvFAdd(_274, float3(1.0)), float3(0.0)), float3(0.4545454680919647216796875)), ViewportParameters.SelectionColorAndStrength.xyz, float3(ViewportParameters.SelectionColorAndStrength.w)), 1.0);
    return out;
}

