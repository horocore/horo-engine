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

struct VSMain_out
{
    float3 out_var_TEXCOORD0 [[user(locn0)]];
    float3 out_var_TEXCOORD1 [[user(locn1)]];
    float4 gl_Position [[position]];
};

struct VSMain_in
{
    float3 in_var_POSITION [[attribute(0)]];
    float3 in_var_NORMAL [[attribute(1)]];
};

vertex VSMain_out VSMain(VSMain_in in [[stage_in]], constant type_ViewportParameters& ViewportParameters [[buffer(0)]])
{
    VSMain_out out = {};
    float4 _47 = float4(in.in_var_POSITION, 1.0);
    float3 _59 = cross(ViewportParameters.ModelToWorld[1].xyz, ViewportParameters.ModelToWorld[2].xyz);
    float _14 = dot(ViewportParameters.ModelToWorld[0].xyz, _59);
    float _67;
    if (abs(_14) > 1.0000000116860974230803549289703e-07)
    {
        _67 = 1.0 / _14;
    }
    else
    {
        _67 = 1.0;
    }
    out.gl_Position = spvFMulMatrixVector(ViewportParameters.Mvp, _47);
    out.out_var_TEXCOORD0 = spvFMulMatrixVector(ViewportParameters.ModelToWorld, _47).xyz;
    out.out_var_TEXCOORD1 = fast::normalize(spvFAdd(spvFAdd(_59 * in.in_var_NORMAL.x, cross(ViewportParameters.ModelToWorld[2].xyz, ViewportParameters.ModelToWorld[0].xyz) * in.in_var_NORMAL.y), cross(ViewportParameters.ModelToWorld[0].xyz, ViewportParameters.ModelToWorld[1].xyz) * in.in_var_NORMAL.z) * _67);
    return out;
}

