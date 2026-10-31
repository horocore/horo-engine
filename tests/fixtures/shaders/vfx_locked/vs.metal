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
    matrix<T, RCols, LRows> res;
    for (uint i = 0; i < RCols; i++)
    {
        vec<T, RCols> tmp(0);
        for (uint j = 0; j < LCols; j++)
        {
            tmp = fma(vec<T, RCols>(r[i][j]), l[j], tmp);
        }
        res[i] = tmp;
    }
    return res;
}

struct type_VfxInstances
{
    float4x4 ClipFromLocal[128];
    float4 InstanceColor[128];
};

struct type_VfxMaterial
{
    float4 Tint;
    float AlphaCutoff;
};

struct VSMain_out
{
    float2 out_var_TEXCOORD0 [[user(locn0)]];
    float4 out_var_COLOR0 [[user(locn1)]];
    float4 gl_Position [[position]];
};

struct VSMain_in
{
    float3 in_var_POSITION [[attribute(0)]];
    float2 in_var_TEXCOORD0 [[attribute(2)]];
};

vertex VSMain_out VSMain(VSMain_in in [[stage_in]], constant type_VfxInstances& VfxInstances [[buffer(0)]], constant type_VfxMaterial& VfxMaterial [[buffer(1)]], uint gl_InstanceIndex [[instance_id]])
{
    VSMain_out out = {};
    out.gl_Position = spvFMulMatrixVector(VfxInstances.ClipFromLocal[gl_InstanceIndex], float4(in.in_var_POSITION, 1.0));
    out.out_var_TEXCOORD0 = in.in_var_TEXCOORD0;
    out.out_var_COLOR0 = spvFMul(VfxInstances.InstanceColor[gl_InstanceIndex], VfxMaterial.Tint);
    return out;
}

