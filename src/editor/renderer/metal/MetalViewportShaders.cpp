#include "MetalViewportShaders.h"

namespace Horo::Editor {
    namespace {
        constexpr char viewportShaderSource[] = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { packed_float3 position; packed_float3 normal; packed_float2 uv; };
struct SelectionStyle { packed_float3 color; float strength; };
struct Light { float4 positionKind; float4 directionRange; float4 colorIntensity; float4 cone; };
struct SceneLighting {
    Light lights[16];
    packed_float3 cameraPosition;
    uint lightCount;
    uint shadowEnabled;
    uint shadowLightIndex;
    uint shadowCascadeCount;
    float padding;
    float4x4 shadowViewProjection[4];
    float4 cascadeSplits[4];
    float4 atlasRects[4];
    float4 cameraForward;
    float4 shadowBiasFilter;
    float2 shadowTexel;
    float2 shadowPadding;
};
struct ObjectTransforms { float4x4 mvp; float4x4 model; };
struct VertexOut {
    float4 position [[position]];
    float3 worldPosition;
    float3 worldNormal;
};
struct GridVertexOut { float4 position [[position]]; };
vertex VertexOut viewport_vertex(uint vertexId [[vertex_id]],
                                 const device Vertex* vertices [[buffer(0)]],
                                 constant ObjectTransforms& transforms [[buffer(1)]])
{
    VertexOut output;
    float4 worldPosition = transforms.model * float4(vertices[vertexId].position, 1.0);
    float3x3 model3x3 = float3x3(transforms.model[0].xyz, transforms.model[1].xyz,
                                transforms.model[2].xyz);
    float3 inverseRow0 = cross(model3x3[1], model3x3[2]);
    float3 inverseRow1 = cross(model3x3[2], model3x3[0]);
    float3 inverseRow2 = cross(model3x3[0], model3x3[1]);
    float determinant = dot(model3x3[0], inverseRow0);
    float inverseDeterminant = abs(determinant) > 0.0000001 ? 1.0 / determinant : 1.0;
    float3x3 normalMatrix = float3x3(inverseRow0 * inverseDeterminant,
                                     inverseRow1 * inverseDeterminant,
                                     inverseRow2 * inverseDeterminant);
    output.position = transforms.mvp * float4(vertices[vertexId].position, 1.0);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = normalize(normalMatrix * float3(vertices[vertexId].normal));
    return output;
}
vertex float4 viewport_shadow_vertex(uint vertexId [[vertex_id]],
                                     const device Vertex* vertices [[buffer(0)]],
                                     constant float4x4& shadowMvp [[buffer(1)]])
{
    return shadowMvp * float4(vertices[vertexId].position, 1.0);
}
vertex GridVertexOut viewport_grid_vertex(uint vertexId [[vertex_id]],
                                          const device packed_float3* vertices [[buffer(0)]],
                                          constant float4x4& viewProjection [[buffer(1)]])
{
    GridVertexOut output;
    output.position = viewProjection * float4(float3(vertices[vertexId]), 1.0);
    return output;
}
float SampleCascade(uint cascade, float3 worldPosition, float3 normal, float3 lightDirection, constant SceneLighting& scene, depth2d<float> shadowMap, sampler shadowSampler)
{
    float3 localized = worldPosition - float3(scene.cameraPosition) + normal * scene.shadowBiasFilter.z;
    float4 clip = scene.shadowViewProjection[cascade] * float4(localized, 1.0);
    float3 coordinate = clip.xyz / clip.w * float4(0.5, -0.5, 1.0, 0.0).xyz + float3(0.5, 0.5, float4(0.5, -0.5, 1.0, 0.0).w);
    if (any(coordinate <= 0.0) || any(coordinate >= 1.0)) return 1.0;
    float4 rect = scene.atlasRects[cascade];
    float2 uv = coordinate.xy * rect.xy + rect.zw;
    float2 texel = scene.shadowTexel;
    float2 minimum = rect.zw + texel * 0.5;
    float2 maximum = rect.zw + rect.xy - texel * 0.5;
    float bias = scene.shadowBiasFilter.x + scene.shadowBiasFilter.y * (1.0 - saturate(dot(normal, lightDirection)));
    int radius = (int)scene.shadowBiasFilter.w;
    float visibility = 0.0;
    float taps = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            if (abs(x) <= radius && abs(y) <= radius)
            {
                float2 sampleUv = clamp(uv + float2(x, y) * texel, minimum, maximum);
                float closest = shadowMap.sample(shadowSampler, sampleUv);
                visibility += coordinate.z - bias <= closest ? 1.0 : 0.0;
                taps += 1.0;
            }
    return visibility / max(taps, 1.0);
}
float DirectionalShadow(float3 worldPosition, float3 normal, float3 lightDirection, constant SceneLighting& scene, depth2d<float> shadowMap, sampler shadowSampler)
{
    if (scene.shadowEnabled == 0.0) return 1.0;
    float distance = dot(worldPosition - float3(scene.cameraPosition), scene.cameraForward.xyz);
    uint count = (uint)scene.shadowCascadeCount;
    uint cascade = count;
    for (uint index = 0; index < count; ++index)
        if (distance <= scene.cascadeSplits[index].x) { cascade = index; break; }
    if (cascade >= count) return 1.0;
    if (distance < scene.cascadeSplits[0].z) return 1.0;
    float visibility = SampleCascade(cascade, worldPosition, normal, lightDirection, scene, shadowMap, shadowSampler);
    float blend = saturate((distance - scene.cascadeSplits[cascade].y) / max(scene.cascadeSplits[cascade].x - scene.cascadeSplits[cascade].y, 0.0001));
    if (cascade + 1 < count && blend > 0.0)
        visibility = mix(visibility, SampleCascade(cascade + 1, worldPosition, normal, lightDirection, scene, shadowMap, shadowSampler), blend);
    return visibility;
}

fragment float4 viewport_fragment(VertexOut input [[stage_in]],
                                  constant SelectionStyle& selection [[buffer(0)]],
                                  constant SceneLighting& scene [[buffer(1)]],
                                  depth2d<float> shadowMap [[texture(0)]],
                                  sampler shadowSampler [[sampler(0)]])
{
    float3 normal = normalize(input.worldNormal);
    float3 viewDirection = normalize(float3(scene.cameraPosition) - input.worldPosition);
    float3 baseColor = float3(0.68, 0.70, 0.74);
    float3 lighting = baseColor * 0.08;
    for (uint index = 0; index < scene.lightCount; ++index)
    {
        constant Light& light = scene.lights[index];
        uint kind = uint(light.positionKind.w + 0.5);
        float3 lightDirection;
        float attenuation = 1.0;
        if (kind == 0)
        {
            lightDirection = normalize(-light.directionRange.xyz);
        }
        else
        {
            float3 toLight = light.positionKind.xyz - input.worldPosition;
            float distanceToLight = length(toLight);
            lightDirection = distanceToLight > 0.0001 ? toLight / distanceToLight : normal;
            float range = max(light.directionRange.w, 0.0001);
            float normalizedDistance = distanceToLight / range;
            float rangeFade = max(1.0 - normalizedDistance * normalizedDistance, 0.0);
            attenuation = rangeFade * rangeFade;
            if (kind == 2)
            {
                float coneCosine = dot(normalize(light.directionRange.xyz), -lightDirection);
                attenuation *= smoothstep(light.cone.y, light.cone.x, coneCosine);
            }
        }
        float diffuse = max(dot(normal, lightDirection), 0.0);
        float3 halfDirection = normalize(lightDirection + viewDirection);
        float specular = pow(max(dot(normal, halfDirection), 0.0), 48.0) * 0.18;
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
        float visibility = index == scene.shadowLightIndex ? DirectionalShadow(input.worldPosition, normal, lightDirection, scene, shadowMap, shadowSampler) : 1.0;
        lighting += radiance * (baseColor * diffuse + specular) * visibility;
    }
    float3 mapped = lighting / (lighting + 1.0);
    float3 displayColor = pow(max(mapped, 0.0), float3(1.0 / 2.2));
    return float4(mix(displayColor, float3(selection.color), selection.strength), 1.0);
}
fragment float4 viewport_grid_fragment(GridVertexOut input [[stage_in]],
                                       constant SelectionStyle& style [[buffer(0)]])
{
    (void)input;
    return float4(style.color, 1.0);
}
)metal";
    }

    const char *MetalViewportShaderSource() noexcept {
        return viewportShaderSource;
    }
}  // namespace Horo::Editor
