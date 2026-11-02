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

struct type_ByteAddressBuffer
{
    uint _m0[1];
};

struct type_RWByteAddressBuffer
{
    uint _m0[1];
};

struct type_SimulationParameters
{
    uint4 LayoutAndForce;
    uint4 Step;
    uint4 SeedAndActivationScope;
    uint4 ActivationAndEmitterScope;
    uint4 EmitterAndSourceGeneration;
    uint4 TargetGenerationAndSchema;
};

kernel void SimulateParticles(device type_ByteAddressBuffer& SourceParticles [[buffer(0)]], device type_RWByteAddressBuffer& DestinationParticles [[buffer(1)]], device type_ByteAddressBuffer& ForceWords [[buffer(2)]], device type_RWByteAddressBuffer& Observations [[buffer(3)]], constant type_SimulationParameters& SimulationParameters [[buffer(4)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    do
    {
        if (gl_GlobalInvocationID.x >= SimulationParameters.LayoutAndForce.y)
        {
            break;
        }
        if (gl_GlobalInvocationID.x >= SimulationParameters.LayoutAndForce.x)
        {
            for (uint _819 = 0u; _819 < SimulationParameters.LayoutAndForce.z; )
            {
                DestinationParticles._m0[(((_819 * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u] = 0u;
                _819++;
                continue;
            }
            break;
        }
        for (uint _828 = 0u; _828 < SimulationParameters.LayoutAndForce.z; )
        {
            uint _834 = (((_828 * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            DestinationParticles._m0[_834] = SourceParticles._m0[_834];
            _828++;
            continue;
        }
        uint _838 = (((16u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
        if (SourceParticles._m0[_838] != 0u)
        {
            uint _844 = (gl_GlobalInvocationID.x * 4u) >> 2u;
            uint _846 = SourceParticles._m0[_844];
            uint _848 = ((SimulationParameters.LayoutAndForce.y + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _850 = SourceParticles._m0[_848];
            uint _852 = (((2u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _854 = SourceParticles._m0[_852];
            float3 _856 = float3(as_type<float>(_846), as_type<float>(_850), as_type<float>(_854));
            uint _857 = (((3u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _859 = SourceParticles._m0[_857];
            uint _861 = (((4u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _863 = SourceParticles._m0[_861];
            uint _865 = (((5u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _867 = SourceParticles._m0[_865];
            uint _870 = (((14u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _873 = (((15u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            float3 _877;
            _877 = float3(0.0);
            float3 _15;
            for (uint _879 = 0u; _879 < SimulationParameters.LayoutAndForce.w; _877 = _15, _879++)
            {
                float3 _2030;
                do
                {
                    uint _55 = _879 * 48u;
                    uint _887 = _55 >> 2u;
                    uint _890 = (_55 + 4u) >> 2u;
                    float _896 = as_type<float>(ForceWords._m0[(_55 + 8u) >> 2u]);
                    uint _901 = (_55 + 16u) >> 2u;
                    if (ForceWords._m0[_887] < 2u)
                    {
                        _2030 = as_type<float3>(uint3(ForceWords._m0[_901], ForceWords._m0[_901 + 1u], ForceWords._m0[_901 + 2u])) * _896;
                        break;
                    }
                    if (ForceWords._m0[_887] == 2u)
                    {
                        uint _920 = (_55 + 32u) >> 2u;
                        float3 _67 = spvFSub(as_type<float3>(uint3(ForceWords._m0[_920], ForceWords._m0[_920 + 1u], ForceWords._m0[_920 + 2u])), _856);
                        float _929 = _67.x;
                        float _930 = _67.y;
                        float _931 = _67.z;
                        float _72 = spvFAdd(spvFAdd(spvFMul(_929, _929), spvFMul(_930, _930)), spvFMul(_931, _931));
                        if (_72 <= 1.1920928955078125e-07)
                        {
                            _2030 = float3(0.0);
                            break;
                        }
                        float _935 = precise::sqrt(_72);
                        _2030 = (_67 / float3(_935)) * spvFMul(_896, 1.0 / spvFAdd(1.0, spvFMul(as_type<float>(ForceWords._m0[(_55 + 12u) >> 2u]), _935)));
                        break;
                    }
                    uint _85 = SimulationParameters.SeedAndActivationScope.x + 2135587861u;
                    uint _87 = (SimulationParameters.SeedAndActivationScope.y + 2654435769u) + uint(_85 < SimulationParameters.SeedAndActivationScope.x);
                    uint2 _949 = uint2(_85, _87) ^ uint2((_85 >> 30u) | (_87 << 2u), _87 >> 30u);
                    uint _950 = _949.x;
                    uint _951 = _950 & 65535u;
                    uint _952 = _950 >> 16u;
                    uint _88 = _951 * 58809u;
                    uint _90 = (_952 * 58809u) + (_88 >> 16u);
                    uint _92 = (_951 * 7396u) + (_90 & 65535u);
                    uint _959 = (_92 << 16u) | (_88 & 65535u);
                    uint _99 = ((((_952 * 7396u) + (_90 >> 16u)) + (_92 >> 16u)) + (_950 * 3210233709u)) + (_949.y * 484763065u);
                    uint2 _967 = uint2(_959, _99) ^ uint2((_959 >> 27u) | (_99 << 5u), _99 >> 27u);
                    uint _968 = _967.x;
                    uint _969 = _968 & 65535u;
                    uint _970 = _968 >> 16u;
                    uint _100 = _969 * 4587u;
                    uint _102 = (_970 * 4587u) + (_100 >> 16u);
                    uint _104 = (_969 * 4913u) + (_102 & 65535u);
                    uint _977 = (_104 << 16u) | (_100 & 65535u);
                    uint _111 = ((((_970 * 4913u) + (_102 >> 16u)) + (_104 >> 16u)) + (_968 * 2496678331u)) + (_967.y * 321982955u);
                    uint2 _985 = uint2(_977, _111) ^ uint2((_977 >> 31u) | (_111 << 1u), _111 >> 31u);
                    uint _112 = SimulationParameters.SeedAndActivationScope.z + 2135587861u;
                    uint _115 = _112 + (_985.x << 6u);
                    uint _118 = _115 + ((_985.x >> 2u) | (_985.y << 30u));
                    uint _121 = _118 + 2135587861u;
                    uint _123 = (((((((SimulationParameters.SeedAndActivationScope.w + 2654435769u) + uint(_112 < SimulationParameters.SeedAndActivationScope.z)) + ((_985.y << 6u) | (_985.x >> 26u))) + uint(_115 < _112)) + (_985.y >> 2u)) + uint(_118 < _115)) + 2654435769u) + uint(_121 < _118);
                    uint2 _1012 = uint2(_121, _123) ^ uint2((_121 >> 30u) | (_123 << 2u), _123 >> 30u);
                    uint _1013 = _1012.x;
                    uint _1014 = _1013 & 65535u;
                    uint _1015 = _1013 >> 16u;
                    uint _124 = _1014 * 58809u;
                    uint _126 = (_1015 * 58809u) + (_124 >> 16u);
                    uint _128 = (_1014 * 7396u) + (_126 & 65535u);
                    uint _1022 = (_128 << 16u) | (_124 & 65535u);
                    uint _135 = ((((_1015 * 7396u) + (_126 >> 16u)) + (_128 >> 16u)) + (_1013 * 3210233709u)) + (_1012.y * 484763065u);
                    uint2 _1030 = uint2(_1022, _135) ^ uint2((_1022 >> 27u) | (_135 << 5u), _135 >> 27u);
                    uint _1031 = _1030.x;
                    uint _1032 = _1031 & 65535u;
                    uint _1033 = _1031 >> 16u;
                    uint _136 = _1032 * 4587u;
                    uint _138 = (_1033 * 4587u) + (_136 >> 16u);
                    uint _140 = (_1032 * 4913u) + (_138 & 65535u);
                    uint _1040 = (_140 << 16u) | (_136 & 65535u);
                    uint _147 = ((((_1033 * 4913u) + (_138 >> 16u)) + (_140 >> 16u)) + (_1031 * 2496678331u)) + (_1030.y * 321982955u);
                    uint2 _1049 = _985 ^ (uint2(_1040, _147) ^ uint2((_1040 >> 31u) | (_147 << 1u), _147 >> 31u));
                    uint _148 = SimulationParameters.ActivationAndEmitterScope.x + 2135587861u;
                    uint _1054 = _1049.x;
                    uint _1056 = _1049.y;
                    uint _150 = _148 + (_1054 << 6u);
                    uint _153 = _150 + ((_1054 >> 2u) | (_1056 << 30u));
                    uint _156 = _153 + 2135587861u;
                    uint _158 = (((((((_148 < SimulationParameters.ActivationAndEmitterScope.x) ? 2654435770u : 2654435769u) + ((_1056 << 6u) | (_1054 >> 26u))) + uint(_150 < _148)) + (_1056 >> 2u)) + uint(_153 < _150)) + 2654435769u) + uint(_156 < _153);
                    uint2 _1076 = uint2(_156, _158) ^ uint2((_156 >> 30u) | (_158 << 2u), _158 >> 30u);
                    uint _1077 = _1076.x;
                    uint _1078 = _1077 & 65535u;
                    uint _1079 = _1077 >> 16u;
                    uint _159 = _1078 * 58809u;
                    uint _161 = (_1079 * 58809u) + (_159 >> 16u);
                    uint _163 = (_1078 * 7396u) + (_161 & 65535u);
                    uint _1086 = (_163 << 16u) | (_159 & 65535u);
                    uint _170 = ((((_1079 * 7396u) + (_161 >> 16u)) + (_163 >> 16u)) + (_1077 * 3210233709u)) + (_1076.y * 484763065u);
                    uint2 _1094 = uint2(_1086, _170) ^ uint2((_1086 >> 27u) | (_170 << 5u), _170 >> 27u);
                    uint _1095 = _1094.x;
                    uint _1096 = _1095 & 65535u;
                    uint _1097 = _1095 >> 16u;
                    uint _171 = _1096 * 4587u;
                    uint _173 = (_1097 * 4587u) + (_171 >> 16u);
                    uint _175 = (_1096 * 4913u) + (_173 & 65535u);
                    uint _1104 = (_175 << 16u) | (_171 & 65535u);
                    uint _182 = ((((_1097 * 4913u) + (_173 >> 16u)) + (_175 >> 16u)) + (_1095 * 2496678331u)) + (_1094.y * 321982955u);
                    uint2 _1113 = _1049 ^ (uint2(_1104, _182) ^ uint2((_1104 >> 31u) | (_182 << 1u), _182 >> 31u));
                    uint _183 = SimulationParameters.ActivationAndEmitterScope.y + 2135587861u;
                    uint _1117 = _1113.x;
                    uint _1119 = _1113.y;
                    uint _185 = _183 + (_1117 << 6u);
                    uint _188 = _185 + ((_1117 >> 2u) | (_1119 << 30u));
                    uint _191 = _188 + 2135587861u;
                    uint _193 = (((((((_183 < SimulationParameters.ActivationAndEmitterScope.y) ? 2654435770u : 2654435769u) + ((_1119 << 6u) | (_1117 >> 26u))) + uint(_185 < _183)) + (_1119 >> 2u)) + uint(_188 < _185)) + 2654435769u) + uint(_191 < _188);
                    uint2 _1139 = uint2(_191, _193) ^ uint2((_191 >> 30u) | (_193 << 2u), _193 >> 30u);
                    uint _1141 = _1139.x & 65535u;
                    uint _1142 = _1139.x >> 16u;
                    uint _194 = _1141 * 58809u;
                    uint _196 = (_1142 * 58809u) + (_194 >> 16u);
                    uint _198 = (_1141 * 7396u) + (_196 & 65535u);
                    uint _1149 = (_198 << 16u) | (_194 & 65535u);
                    uint _205 = ((((_1142 * 7396u) + (_196 >> 16u)) + (_198 >> 16u)) + (_1139.x * 3210233709u)) + (_1139.y * 484763065u);
                    uint2 _1157 = uint2(_1149, _205) ^ uint2((_1149 >> 27u) | (_205 << 5u), _205 >> 27u);
                    uint _1158 = _1157.x;
                    uint _1159 = _1158 & 65535u;
                    uint _1160 = _1158 >> 16u;
                    uint _206 = _1159 * 4587u;
                    uint _208 = (_1160 * 4587u) + (_206 >> 16u);
                    uint _210 = (_1159 * 4913u) + (_208 & 65535u);
                    uint _1167 = (_210 << 16u) | (_206 & 65535u);
                    uint _217 = ((((_1160 * 4913u) + (_208 >> 16u)) + (_210 >> 16u)) + (_1158 * 2496678331u)) + (_1157.y * 321982955u);
                    uint2 _1176 = _1113 ^ (uint2(_1167, _217) ^ uint2((_1167 >> 31u) | (_217 << 1u), _217 >> 31u));
                    uint _218 = SimulationParameters.ActivationAndEmitterScope.z + 2135587861u;
                    uint _221 = _218 + (_1176.x << 6u);
                    uint _224 = _221 + ((_1176.x >> 2u) | (_1176.y << 30u));
                    uint _227 = _224 + 2135587861u;
                    uint _229 = (((((((SimulationParameters.ActivationAndEmitterScope.w + 2654435769u) + uint(_218 < SimulationParameters.ActivationAndEmitterScope.z)) + ((_1176.y << 6u) | (_1176.x >> 26u))) + uint(_221 < _218)) + (_1176.y >> 2u)) + uint(_224 < _221)) + 2654435769u) + uint(_227 < _224);
                    uint2 _1204 = uint2(_227, _229) ^ uint2((_227 >> 30u) | (_229 << 2u), _229 >> 30u);
                    uint _1205 = _1204.x;
                    uint _1206 = _1205 & 65535u;
                    uint _1207 = _1205 >> 16u;
                    uint _230 = _1206 * 58809u;
                    uint _232 = (_1207 * 58809u) + (_230 >> 16u);
                    uint _234 = (_1206 * 7396u) + (_232 & 65535u);
                    uint _1214 = (_234 << 16u) | (_230 & 65535u);
                    uint _241 = ((((_1207 * 7396u) + (_232 >> 16u)) + (_234 >> 16u)) + (_1205 * 3210233709u)) + (_1204.y * 484763065u);
                    uint2 _1222 = uint2(_1214, _241) ^ uint2((_1214 >> 27u) | (_241 << 5u), _241 >> 27u);
                    uint _1223 = _1222.x;
                    uint _1224 = _1223 & 65535u;
                    uint _1225 = _1223 >> 16u;
                    uint _242 = _1224 * 4587u;
                    uint _244 = (_1225 * 4587u) + (_242 >> 16u);
                    uint _246 = (_1224 * 4913u) + (_244 & 65535u);
                    uint _1232 = (_246 << 16u) | (_242 & 65535u);
                    uint _253 = ((((_1225 * 4913u) + (_244 >> 16u)) + (_246 >> 16u)) + (_1223 * 2496678331u)) + (_1222.y * 321982955u);
                    uint2 _1241 = _1176 ^ (uint2(_1232, _253) ^ uint2((_1232 >> 31u) | (_253 << 1u), _253 >> 31u));
                    uint _254 = SimulationParameters.EmitterAndSourceGeneration.x + 2135587861u;
                    uint _1245 = _1241.x;
                    uint _1247 = _1241.y;
                    uint _256 = _254 + (_1245 << 6u);
                    uint _259 = _256 + ((_1245 >> 2u) | (_1247 << 30u));
                    uint _262 = _259 + 2135587861u;
                    uint _264 = (((((((_254 < SimulationParameters.EmitterAndSourceGeneration.x) ? 2654435770u : 2654435769u) + ((_1247 << 6u) | (_1245 >> 26u))) + uint(_256 < _254)) + (_1247 >> 2u)) + uint(_259 < _256)) + 2654435769u) + uint(_262 < _259);
                    uint2 _1267 = uint2(_262, _264) ^ uint2((_262 >> 30u) | (_264 << 2u), _264 >> 30u);
                    uint _1268 = _1267.x;
                    uint _1269 = _1268 & 65535u;
                    uint _1270 = _1268 >> 16u;
                    uint _265 = _1269 * 58809u;
                    uint _267 = (_1270 * 58809u) + (_265 >> 16u);
                    uint _269 = (_1269 * 7396u) + (_267 & 65535u);
                    uint _1277 = (_269 << 16u) | (_265 & 65535u);
                    uint _276 = ((((_1270 * 7396u) + (_267 >> 16u)) + (_269 >> 16u)) + (_1268 * 3210233709u)) + (_1267.y * 484763065u);
                    uint2 _1285 = uint2(_1277, _276) ^ uint2((_1277 >> 27u) | (_276 << 5u), _276 >> 27u);
                    uint _1286 = _1285.x;
                    uint _1287 = _1286 & 65535u;
                    uint _1288 = _1286 >> 16u;
                    uint _277 = _1287 * 4587u;
                    uint _279 = (_1288 * 4587u) + (_277 >> 16u);
                    uint _281 = (_1287 * 4913u) + (_279 & 65535u);
                    uint _1295 = (_281 << 16u) | (_277 & 65535u);
                    uint _288 = ((((_1288 * 4913u) + (_279 >> 16u)) + (_281 >> 16u)) + (_1286 * 2496678331u)) + (_1285.y * 321982955u);
                    uint2 _1304 = _1241 ^ (uint2(_1295, _288) ^ uint2((_1295 >> 31u) | (_288 << 1u), _288 >> 31u));
                    uint _289 = SimulationParameters.EmitterAndSourceGeneration.y + 2135587861u;
                    uint _1308 = _1304.x;
                    uint _1310 = _1304.y;
                    uint _291 = _289 + (_1308 << 6u);
                    uint _294 = _291 + ((_1308 >> 2u) | (_1310 << 30u));
                    uint _297 = _294 + 2135587861u;
                    uint _299 = (((((((_289 < SimulationParameters.EmitterAndSourceGeneration.y) ? 2654435770u : 2654435769u) + ((_1310 << 6u) | (_1308 >> 26u))) + uint(_291 < _289)) + (_1310 >> 2u)) + uint(_294 < _291)) + 2654435769u) + uint(_297 < _294);
                    uint2 _1330 = uint2(_297, _299) ^ uint2((_297 >> 30u) | (_299 << 2u), _299 >> 30u);
                    uint _1332 = _1330.x & 65535u;
                    uint _1333 = _1330.x >> 16u;
                    uint _300 = _1332 * 58809u;
                    uint _302 = (_1333 * 58809u) + (_300 >> 16u);
                    uint _304 = (_1332 * 7396u) + (_302 & 65535u);
                    uint _1340 = (_304 << 16u) | (_300 & 65535u);
                    uint _311 = ((((_1333 * 7396u) + (_302 >> 16u)) + (_304 >> 16u)) + (_1330.x * 3210233709u)) + (_1330.y * 484763065u);
                    uint2 _1348 = uint2(_1340, _311) ^ uint2((_1340 >> 27u) | (_311 << 5u), _311 >> 27u);
                    uint _1349 = _1348.x;
                    uint _1350 = _1349 & 65535u;
                    uint _1351 = _1349 >> 16u;
                    uint _312 = _1350 * 4587u;
                    uint _314 = (_1351 * 4587u) + (_312 >> 16u);
                    uint _316 = (_1350 * 4913u) + (_314 & 65535u);
                    uint _1358 = (_316 << 16u) | (_312 & 65535u);
                    uint _323 = ((((_1351 * 4913u) + (_314 >> 16u)) + (_316 >> 16u)) + (_1349 * 2496678331u)) + (_1348.y * 321982955u);
                    uint2 _1367 = _1304 ^ (uint2(_1358, _323) ^ uint2((_1358 >> 31u) | (_323 << 1u), _323 >> 31u));
                    uint _324 = SourceParticles._m0[_870] + 2135587861u;
                    uint _327 = _324 + (_1367.x << 6u);
                    uint _330 = _327 + ((_1367.x >> 2u) | (_1367.y << 30u));
                    uint _333 = _330 + 2135587861u;
                    uint _335 = (((((((SourceParticles._m0[_873] + 2654435769u) + uint(_324 < SourceParticles._m0[_870])) + ((_1367.y << 6u) | (_1367.x >> 26u))) + uint(_327 < _324)) + (_1367.y >> 2u)) + uint(_330 < _327)) + 2654435769u) + uint(_333 < _330);
                    uint2 _1392 = uint2(_333, _335) ^ uint2((_333 >> 30u) | (_335 << 2u), _335 >> 30u);
                    uint _1393 = _1392.x;
                    uint _1394 = _1393 & 65535u;
                    uint _1395 = _1393 >> 16u;
                    uint _336 = _1394 * 58809u;
                    uint _338 = (_1395 * 58809u) + (_336 >> 16u);
                    uint _340 = (_1394 * 7396u) + (_338 & 65535u);
                    uint _1402 = (_340 << 16u) | (_336 & 65535u);
                    uint _347 = ((((_1395 * 7396u) + (_338 >> 16u)) + (_340 >> 16u)) + (_1393 * 3210233709u)) + (_1392.y * 484763065u);
                    uint2 _1410 = uint2(_1402, _347) ^ uint2((_1402 >> 27u) | (_347 << 5u), _347 >> 27u);
                    uint _1411 = _1410.x;
                    uint _1412 = _1411 & 65535u;
                    uint _1413 = _1411 >> 16u;
                    uint _348 = _1412 * 4587u;
                    uint _350 = (_1413 * 4587u) + (_348 >> 16u);
                    uint _352 = (_1412 * 4913u) + (_350 & 65535u);
                    uint _1420 = (_352 << 16u) | (_348 & 65535u);
                    uint _359 = ((((_1413 * 4913u) + (_350 >> 16u)) + (_352 >> 16u)) + (_1411 * 2496678331u)) + (_1410.y * 321982955u);
                    uint2 _1429 = _1367 ^ (uint2(_1420, _359) ^ uint2((_1420 >> 31u) | (_359 << 1u), _359 >> 31u));
                    uint _360 = ForceWords._m0[_890] + 2135587861u;
                    uint _1431 = _1429.x;
                    uint _1433 = _1429.y;
                    uint _362 = _360 + (_1431 << 6u);
                    uint _365 = _362 + ((_1431 >> 2u) | (_1433 << 30u));
                    uint _368 = _365 + 2135587861u;
                    uint _370 = (((((((_360 < ForceWords._m0[_890]) ? 2654435770u : 2654435769u) + ((_1433 << 6u) | (_1431 >> 26u))) + uint(_362 < _360)) + (_1433 >> 2u)) + uint(_365 < _362)) + 2654435769u) + uint(_368 < _365);
                    uint2 _1453 = uint2(_368, _370) ^ uint2((_368 >> 30u) | (_370 << 2u), _370 >> 30u);
                    uint _1454 = _1453.x;
                    uint _1455 = _1454 & 65535u;
                    uint _1456 = _1454 >> 16u;
                    uint _371 = _1455 * 58809u;
                    uint _373 = (_1456 * 58809u) + (_371 >> 16u);
                    uint _375 = (_1455 * 7396u) + (_373 & 65535u);
                    uint _1463 = (_375 << 16u) | (_371 & 65535u);
                    uint _382 = ((((_1456 * 7396u) + (_373 >> 16u)) + (_375 >> 16u)) + (_1454 * 3210233709u)) + (_1453.y * 484763065u);
                    uint2 _1471 = uint2(_1463, _382) ^ uint2((_1463 >> 27u) | (_382 << 5u), _382 >> 27u);
                    uint _1472 = _1471.x;
                    uint _1473 = _1472 & 65535u;
                    uint _1474 = _1472 >> 16u;
                    uint _383 = _1473 * 4587u;
                    uint _385 = (_1474 * 4587u) + (_383 >> 16u);
                    uint _387 = (_1473 * 4913u) + (_385 & 65535u);
                    uint _1481 = (_387 << 16u) | (_383 & 65535u);
                    uint _394 = ((((_1474 * 4913u) + (_385 >> 16u)) + (_387 >> 16u)) + (_1472 * 2496678331u)) + (_1471.y * 321982955u);
                    uint2 _1490 = _1429 ^ (uint2(_1481, _394) ^ uint2((_1481 >> 31u) | (_394 << 1u), _394 >> 31u));
                    uint _1491 = _1490.x;
                    uint _1492 = _1491 << 6u;
                    uint _1493 = _1490.y;
                    uint _395 = 2135587861u + _1492;
                    uint _396 = 2654435769u + ((_1493 << 6u) | (_1491 >> 26u));
                    uint _1501 = (_1491 >> 2u) | (_1493 << 30u);
                    uint _1502 = _1493 >> 2u;
                    uint _398 = _395 + _1501;
                    uint _401 = _398 + 2135587861u;
                    uint _403 = ((((_396 + uint(_395 < 2135587861u)) + _1502) + uint(_398 < _395)) + 2654435769u) + uint(_401 < _398);
                    uint2 _1513 = uint2(_401, _403) ^ uint2((_401 >> 30u) | (_403 << 2u), _403 >> 30u);
                    uint _1515 = _1513.x & 65535u;
                    uint _1516 = _1513.x >> 16u;
                    uint _404 = _1515 * 58809u;
                    uint _406 = (_1516 * 58809u) + (_404 >> 16u);
                    uint _408 = (_1515 * 7396u) + (_406 & 65535u);
                    uint _1523 = (_408 << 16u) | (_404 & 65535u);
                    uint _415 = ((((_1516 * 7396u) + (_406 >> 16u)) + (_408 >> 16u)) + (_1513.x * 3210233709u)) + (_1513.y * 484763065u);
                    uint2 _1531 = uint2(_1523, _415) ^ uint2((_1523 >> 27u) | (_415 << 5u), _415 >> 27u);
                    uint _1532 = _1531.x;
                    uint _1533 = _1532 & 65535u;
                    uint _1534 = _1532 >> 16u;
                    uint _416 = _1533 * 4587u;
                    uint _418 = (_1534 * 4587u) + (_416 >> 16u);
                    uint _420 = (_1533 * 4913u) + (_418 & 65535u);
                    uint _1541 = (_420 << 16u) | (_416 & 65535u);
                    uint _427 = ((((_1534 * 4913u) + (_418 >> 16u)) + (_420 >> 16u)) + (_1532 * 2496678331u)) + (_1531.y * 321982955u);
                    uint2 _1550 = _1490 ^ (uint2(_1541, _427) ^ uint2((_1541 >> 31u) | (_427 << 1u), _427 >> 31u));
                    uint _428 = SimulationParameters.Step.y + 2135587861u;
                    uint _430 = (SimulationParameters.Step.z + 2654435769u) + uint(_428 < SimulationParameters.Step.y);
                    uint _431 = _428 + (_1550.x << 6u);
                    uint _434 = _431 + ((_1550.x >> 2u) | (_1550.y << 30u));
                    uint _437 = _434 + 2135587861u;
                    uint _439 = (((((_430 + ((_1550.y << 6u) | (_1550.x >> 26u))) + uint(_431 < _428)) + (_1550.y >> 2u)) + uint(_434 < _431)) + 2654435769u) + uint(_437 < _434);
                    uint2 _1579 = uint2(_437, _439) ^ uint2((_437 >> 30u) | (_439 << 2u), _439 >> 30u);
                    uint _1580 = _1579.x;
                    uint _1581 = _1580 & 65535u;
                    uint _1582 = _1580 >> 16u;
                    uint _440 = _1581 * 58809u;
                    uint _442 = (_1582 * 58809u) + (_440 >> 16u);
                    uint _444 = (_1581 * 7396u) + (_442 & 65535u);
                    uint _1589 = (_444 << 16u) | (_440 & 65535u);
                    uint _451 = ((((_1582 * 7396u) + (_442 >> 16u)) + (_444 >> 16u)) + (_1580 * 3210233709u)) + (_1579.y * 484763065u);
                    uint2 _1597 = uint2(_1589, _451) ^ uint2((_1589 >> 27u) | (_451 << 5u), _451 >> 27u);
                    uint _1598 = _1597.x;
                    uint _1599 = _1598 & 65535u;
                    uint _1600 = _1598 >> 16u;
                    uint _452 = _1599 * 4587u;
                    uint _454 = (_1600 * 4587u) + (_452 >> 16u);
                    uint _456 = (_1599 * 4913u) + (_454 & 65535u);
                    uint _1607 = (_456 << 16u) | (_452 & 65535u);
                    uint _463 = ((((_1600 * 4913u) + (_454 >> 16u)) + (_456 >> 16u)) + (_1598 * 2496678331u)) + (_1597.y * 321982955u);
                    uint2 _1616 = _1550 ^ (uint2(_1607, _463) ^ uint2((_1607 >> 31u) | (_463 << 1u), _463 >> 31u));
                    uint _464 = SimulationParameters.TargetGenerationAndSchema.w + 2135587861u;
                    uint _465 = (_464 < SimulationParameters.TargetGenerationAndSchema.w) ? 2654435770u : 2654435769u;
                    uint _1620 = _1616.x;
                    uint _1622 = _1616.y;
                    uint _466 = _464 + (_1620 << 6u);
                    uint _469 = _466 + ((_1620 >> 2u) | (_1622 << 30u));
                    uint _472 = _469 + 2135587861u;
                    uint _474 = (((((_465 + ((_1622 << 6u) | (_1620 >> 26u))) + uint(_466 < _464)) + (_1622 >> 2u)) + uint(_469 < _466)) + 2654435769u) + uint(_472 < _469);
                    uint2 _1642 = uint2(_472, _474) ^ uint2((_472 >> 30u) | (_474 << 2u), _474 >> 30u);
                    uint _1643 = _1642.x;
                    uint _1644 = _1643 & 65535u;
                    uint _1645 = _1643 >> 16u;
                    uint _475 = _1644 * 58809u;
                    uint _477 = (_1645 * 58809u) + (_475 >> 16u);
                    uint _479 = (_1644 * 7396u) + (_477 & 65535u);
                    uint _1652 = (_479 << 16u) | (_475 & 65535u);
                    uint _486 = ((((_1645 * 7396u) + (_477 >> 16u)) + (_479 >> 16u)) + (_1643 * 3210233709u)) + (_1642.y * 484763065u);
                    uint2 _1660 = uint2(_1652, _486) ^ uint2((_1652 >> 27u) | (_486 << 5u), _486 >> 27u);
                    uint _1661 = _1660.x;
                    uint _1662 = _1661 & 65535u;
                    uint _1663 = _1661 >> 16u;
                    uint _487 = _1662 * 4587u;
                    uint _489 = (_1663 * 4587u) + (_487 >> 16u);
                    uint _491 = (_1662 * 4913u) + (_489 & 65535u);
                    uint _1670 = (_491 << 16u) | (_487 & 65535u);
                    uint _498 = ((((_1663 * 4913u) + (_489 >> 16u)) + (_491 >> 16u)) + (_1661 * 2496678331u)) + (_1660.y * 321982955u);
                    float _83 = 1.0 / 16777216.0;
                    uint _500 = 2135587862u + _1492;
                    uint _502 = _500 + _1501;
                    uint _505 = _502 + 2135587861u;
                    uint _507 = ((((_396 + uint(_500 < 2135587862u)) + _1502) + uint(_502 < _500)) + 2654435769u) + uint(_505 < _502);
                    uint2 _1695 = uint2(_505, _507) ^ uint2((_505 >> 30u) | (_507 << 2u), _507 >> 30u);
                    uint _1697 = _1695.x & 65535u;
                    uint _1698 = _1695.x >> 16u;
                    uint _508 = _1697 * 58809u;
                    uint _510 = (_1698 * 58809u) + (_508 >> 16u);
                    uint _512 = (_1697 * 7396u) + (_510 & 65535u);
                    uint _1705 = (_512 << 16u) | (_508 & 65535u);
                    uint _519 = ((((_1698 * 7396u) + (_510 >> 16u)) + (_512 >> 16u)) + (_1695.x * 3210233709u)) + (_1695.y * 484763065u);
                    uint2 _1713 = uint2(_1705, _519) ^ uint2((_1705 >> 27u) | (_519 << 5u), _519 >> 27u);
                    uint _1714 = _1713.x;
                    uint _1715 = _1714 & 65535u;
                    uint _1716 = _1714 >> 16u;
                    uint _520 = _1715 * 4587u;
                    uint _522 = (_1716 * 4587u) + (_520 >> 16u);
                    uint _524 = (_1715 * 4913u) + (_522 & 65535u);
                    uint _1723 = (_524 << 16u) | (_520 & 65535u);
                    uint _531 = ((((_1716 * 4913u) + (_522 >> 16u)) + (_524 >> 16u)) + (_1714 * 2496678331u)) + (_1713.y * 321982955u);
                    uint2 _1732 = _1490 ^ (uint2(_1723, _531) ^ uint2((_1723 >> 31u) | (_531 << 1u), _531 >> 31u));
                    uint _532 = _428 + (_1732.x << 6u);
                    uint _535 = _532 + ((_1732.x >> 2u) | (_1732.y << 30u));
                    uint _538 = _535 + 2135587861u;
                    uint _540 = (((((_430 + ((_1732.y << 6u) | (_1732.x >> 26u))) + uint(_532 < _428)) + (_1732.y >> 2u)) + uint(_535 < _532)) + 2654435769u) + uint(_538 < _535);
                    uint2 _1755 = uint2(_538, _540) ^ uint2((_538 >> 30u) | (_540 << 2u), _540 >> 30u);
                    uint _1756 = _1755.x;
                    uint _1757 = _1756 & 65535u;
                    uint _1758 = _1756 >> 16u;
                    uint _541 = _1757 * 58809u;
                    uint _543 = (_1758 * 58809u) + (_541 >> 16u);
                    uint _545 = (_1757 * 7396u) + (_543 & 65535u);
                    uint _1765 = (_545 << 16u) | (_541 & 65535u);
                    uint _552 = ((((_1758 * 7396u) + (_543 >> 16u)) + (_545 >> 16u)) + (_1756 * 3210233709u)) + (_1755.y * 484763065u);
                    uint2 _1773 = uint2(_1765, _552) ^ uint2((_1765 >> 27u) | (_552 << 5u), _552 >> 27u);
                    uint _1774 = _1773.x;
                    uint _1775 = _1774 & 65535u;
                    uint _1776 = _1774 >> 16u;
                    uint _553 = _1775 * 4587u;
                    uint _555 = (_1776 * 4587u) + (_553 >> 16u);
                    uint _557 = (_1775 * 4913u) + (_555 & 65535u);
                    uint _1783 = (_557 << 16u) | (_553 & 65535u);
                    uint _564 = ((((_1776 * 4913u) + (_555 >> 16u)) + (_557 >> 16u)) + (_1774 * 2496678331u)) + (_1773.y * 321982955u);
                    uint2 _1792 = _1732 ^ (uint2(_1783, _564) ^ uint2((_1783 >> 31u) | (_564 << 1u), _564 >> 31u));
                    uint _1793 = _1792.x;
                    uint _1795 = _1792.y;
                    uint _565 = _464 + (_1793 << 6u);
                    uint _568 = _565 + ((_1793 >> 2u) | (_1795 << 30u));
                    uint _571 = _568 + 2135587861u;
                    uint _573 = (((((_465 + ((_1795 << 6u) | (_1793 >> 26u))) + uint(_565 < _464)) + (_1795 >> 2u)) + uint(_568 < _565)) + 2654435769u) + uint(_571 < _568);
                    uint2 _1815 = uint2(_571, _573) ^ uint2((_571 >> 30u) | (_573 << 2u), _573 >> 30u);
                    uint _1816 = _1815.x;
                    uint _1817 = _1816 & 65535u;
                    uint _1818 = _1816 >> 16u;
                    uint _574 = _1817 * 58809u;
                    uint _576 = (_1818 * 58809u) + (_574 >> 16u);
                    uint _578 = (_1817 * 7396u) + (_576 & 65535u);
                    uint _1825 = (_578 << 16u) | (_574 & 65535u);
                    uint _585 = ((((_1818 * 7396u) + (_576 >> 16u)) + (_578 >> 16u)) + (_1816 * 3210233709u)) + (_1815.y * 484763065u);
                    uint2 _1833 = uint2(_1825, _585) ^ uint2((_1825 >> 27u) | (_585 << 5u), _585 >> 27u);
                    uint _1834 = _1833.x;
                    uint _1835 = _1834 & 65535u;
                    uint _1836 = _1834 >> 16u;
                    uint _586 = _1835 * 4587u;
                    uint _588 = (_1836 * 4587u) + (_586 >> 16u);
                    uint _590 = (_1835 * 4913u) + (_588 & 65535u);
                    uint _1843 = (_590 << 16u) | (_586 & 65535u);
                    uint _597 = ((((_1836 * 4913u) + (_588 >> 16u)) + (_590 >> 16u)) + (_1834 * 2496678331u)) + (_1833.y * 321982955u);
                    uint _599 = 2135587863u + _1492;
                    uint _601 = _599 + _1501;
                    uint _604 = _601 + 2135587861u;
                    uint _606 = ((((_396 + uint(_599 < 2135587863u)) + _1502) + uint(_601 < _599)) + 2654435769u) + uint(_604 < _601);
                    uint2 _1868 = uint2(_604, _606) ^ uint2((_604 >> 30u) | (_606 << 2u), _606 >> 30u);
                    uint _1870 = _1868.x & 65535u;
                    uint _1871 = _1868.x >> 16u;
                    uint _607 = _1870 * 58809u;
                    uint _609 = (_1871 * 58809u) + (_607 >> 16u);
                    uint _611 = (_1870 * 7396u) + (_609 & 65535u);
                    uint _1878 = (_611 << 16u) | (_607 & 65535u);
                    uint _618 = ((((_1871 * 7396u) + (_609 >> 16u)) + (_611 >> 16u)) + (_1868.x * 3210233709u)) + (_1868.y * 484763065u);
                    uint2 _1886 = uint2(_1878, _618) ^ uint2((_1878 >> 27u) | (_618 << 5u), _618 >> 27u);
                    uint _1887 = _1886.x;
                    uint _1888 = _1887 & 65535u;
                    uint _1889 = _1887 >> 16u;
                    uint _619 = _1888 * 4587u;
                    uint _621 = (_1889 * 4587u) + (_619 >> 16u);
                    uint _623 = (_1888 * 4913u) + (_621 & 65535u);
                    uint _1896 = (_623 << 16u) | (_619 & 65535u);
                    uint _630 = ((((_1889 * 4913u) + (_621 >> 16u)) + (_623 >> 16u)) + (_1887 * 2496678331u)) + (_1886.y * 321982955u);
                    uint2 _1905 = _1490 ^ (uint2(_1896, _630) ^ uint2((_1896 >> 31u) | (_630 << 1u), _630 >> 31u));
                    uint _631 = _428 + (_1905.x << 6u);
                    uint _634 = _631 + ((_1905.x >> 2u) | (_1905.y << 30u));
                    uint _637 = _634 + 2135587861u;
                    uint _639 = (((((_430 + ((_1905.y << 6u) | (_1905.x >> 26u))) + uint(_631 < _428)) + (_1905.y >> 2u)) + uint(_634 < _631)) + 2654435769u) + uint(_637 < _634);
                    uint2 _1928 = uint2(_637, _639) ^ uint2((_637 >> 30u) | (_639 << 2u), _639 >> 30u);
                    uint _1929 = _1928.x;
                    uint _1930 = _1929 & 65535u;
                    uint _1931 = _1929 >> 16u;
                    uint _640 = _1930 * 58809u;
                    uint _642 = (_1931 * 58809u) + (_640 >> 16u);
                    uint _644 = (_1930 * 7396u) + (_642 & 65535u);
                    uint _1938 = (_644 << 16u) | (_640 & 65535u);
                    uint _651 = ((((_1931 * 7396u) + (_642 >> 16u)) + (_644 >> 16u)) + (_1929 * 3210233709u)) + (_1928.y * 484763065u);
                    uint2 _1946 = uint2(_1938, _651) ^ uint2((_1938 >> 27u) | (_651 << 5u), _651 >> 27u);
                    uint _1947 = _1946.x;
                    uint _1948 = _1947 & 65535u;
                    uint _1949 = _1947 >> 16u;
                    uint _652 = _1948 * 4587u;
                    uint _654 = (_1949 * 4587u) + (_652 >> 16u);
                    uint _656 = (_1948 * 4913u) + (_654 & 65535u);
                    uint _1956 = (_656 << 16u) | (_652 & 65535u);
                    uint _663 = ((((_1949 * 4913u) + (_654 >> 16u)) + (_656 >> 16u)) + (_1947 * 2496678331u)) + (_1946.y * 321982955u);
                    uint2 _1965 = _1905 ^ (uint2(_1956, _663) ^ uint2((_1956 >> 31u) | (_663 << 1u), _663 >> 31u));
                    uint _1966 = _1965.x;
                    uint _1968 = _1965.y;
                    uint _664 = _464 + (_1966 << 6u);
                    uint _667 = _664 + ((_1966 >> 2u) | (_1968 << 30u));
                    uint _670 = _667 + 2135587861u;
                    uint _672 = (((((_465 + ((_1968 << 6u) | (_1966 >> 26u))) + uint(_664 < _464)) + (_1968 >> 2u)) + uint(_667 < _664)) + 2654435769u) + uint(_670 < _667);
                    uint2 _1988 = uint2(_670, _672) ^ uint2((_670 >> 30u) | (_672 << 2u), _672 >> 30u);
                    uint _1989 = _1988.x;
                    uint _1990 = _1989 & 65535u;
                    uint _1991 = _1989 >> 16u;
                    uint _673 = _1990 * 58809u;
                    uint _675 = (_1991 * 58809u) + (_673 >> 16u);
                    uint _677 = (_1990 * 7396u) + (_675 & 65535u);
                    uint _1998 = (_677 << 16u) | (_673 & 65535u);
                    uint _684 = ((((_1991 * 7396u) + (_675 >> 16u)) + (_677 >> 16u)) + (_1989 * 3210233709u)) + (_1988.y * 484763065u);
                    uint2 _2006 = uint2(_1998, _684) ^ uint2((_1998 >> 27u) | (_684 << 5u), _684 >> 27u);
                    uint _2007 = _2006.x;
                    uint _2008 = _2007 & 65535u;
                    uint _2009 = _2007 >> 16u;
                    uint _685 = _2008 * 4587u;
                    uint _687 = (_2009 * 4587u) + (_685 >> 16u);
                    uint _689 = (_2008 * 4913u) + (_687 & 65535u);
                    uint _2016 = (_689 << 16u) | (_685 & 65535u);
                    uint _696 = ((((_2009 * 4913u) + (_687 >> 16u)) + (_689 >> 16u)) + (_2007 * 2496678331u)) + (_2006.y * 321982955u);
                    _2030 = spvFSub(float3(spvFMul(float((_1616 ^ (uint2(_1670, _498) ^ uint2((_1670 >> 31u) | (_498 << 1u), _498 >> 31u))).y >> 8u), _83), spvFMul(float((_1792 ^ (uint2(_1843, _597) ^ uint2((_1843 >> 31u) | (_597 << 1u), _597 >> 31u))).y >> 8u), _83), spvFMul(float((_1965 ^ (uint2(_2016, _696) ^ uint2((_2016 >> 31u) | (_696 << 1u), _696 >> 31u))).y >> 8u), _83)) * 2.0, float3(1.0)) * spvFMul(_896, as_type<float>(ForceWords._m0[(_55 + 28u) >> 2u]));
                    break;
                } while(false);
                _15 = spvFAdd(_877, _2030);
            }
            float _2033 = as_type<float>(SimulationParameters.Step.x);
            float3 _18 = spvFAdd(float3(as_type<float>(_859), as_type<float>(_863), as_type<float>(_867)), _877 * _2033);
            float3 _20 = spvFAdd(_856, _18 * _2033);
            DestinationParticles._m0[_844] = as_type<uint>(_20.x);
            DestinationParticles._m0[_848] = as_type<uint>(_20.y);
            DestinationParticles._m0[_852] = as_type<uint>(_20.z);
            DestinationParticles._m0[_857] = as_type<uint>(_18.x);
            DestinationParticles._m0[_861] = as_type<uint>(_18.y);
            DestinationParticles._m0[_865] = as_type<uint>(_18.z);
            uint _2052 = (((8u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _2060 = (((10u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u;
            uint _2062 = SourceParticles._m0[_2060];
            float _24 = spvFAdd(precise::min(as_type<float>(_2062), spvFSub(as_type<float>(2139095039u), _2033)), _2033);
            DestinationParticles._m0[_2052] = as_type<uint>(spvFAdd(as_type<float>(SourceParticles._m0[_2052]), spvFMul(as_type<float>(SourceParticles._m0[(((9u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u]), _2033)));
            DestinationParticles._m0[_2060] = as_type<uint>(_24);
            bool _2082;
            if ((SourceParticles._m0[(((13u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u] & 1u) == 0u)
            {
                _2082 = _24 < as_type<float>(SourceParticles._m0[(((11u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u]);
            }
            else
            {
                _2082 = false;
            }
            DestinationParticles._m0[_838] = uint(_2082);
        }
        if (gl_GlobalInvocationID.x < SimulationParameters.Step.w)
        {
            uint _712 = gl_GlobalInvocationID.x * 80u;
            Observations._m0[_712 >> 2u] = DestinationParticles._m0[(((14u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u];
            Observations._m0[(_712 + 4u) >> 2u] = DestinationParticles._m0[(((15u * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u];
            for (uint _2101 = 0u; _2101 < 14u; )
            {
                Observations._m0[(_712 + ((_2101 + 2u) * 4u)) >> 2u] = DestinationParticles._m0[(((_2101 * SimulationParameters.LayoutAndForce.y) + gl_GlobalInvocationID.x) * 4u) >> 2u];
                _2101++;
                continue;
            }
            Observations._m0[(_712 + 64u) >> 2u] = DestinationParticles._m0[_838];
            uint _2114 = (_712 + 68u) >> 2u;
            Observations._m0[_2114] = 0u;
            Observations._m0[_2114 + 1u] = 0u;
            Observations._m0[_2114 + 2u] = 0u;
        }
        break;
    } while(false);
}

