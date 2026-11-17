#pragma once

namespace Horo::Editor::Detail {
    inline constexpr char ViewportVertexShader[] = R"glsl(#version 150 core
in vec3 aPosition;
in vec3 aNormal;
in vec2 aUv;
out vec3 vWorldPosition;
out vec3 vWorldNormal;
uniform mat4 uMvp;
uniform mat4 uModel;
void main()
{
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    vWorldPosition = worldPosition.xyz;
    mat3 modelBasis = mat3(uModel);
    vec3 inverseRow0 = cross(modelBasis[1], modelBasis[2]);
    vec3 inverseRow1 = cross(modelBasis[2], modelBasis[0]);
    vec3 inverseRow2 = cross(modelBasis[0], modelBasis[1]);
    float determinant = dot(modelBasis[0], inverseRow0);
    float inverseDeterminant = abs(determinant) > 0.0000001 ? 1.0 / determinant : 1.0;
    mat3 normalMatrix = mat3(inverseRow0 * inverseDeterminant,
                             inverseRow1 * inverseDeterminant,
                             inverseRow2 * inverseDeterminant);
    vWorldNormal = normalize(normalMatrix * aNormal);
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)glsl";

    inline constexpr char ViewportFragmentShader[] = R"glsl(#version 150 core
const int MaxLights = 16;
in vec3 vWorldPosition;
in vec3 vWorldNormal;
out vec4 outColor;
uniform vec3 uCameraPosition;
uniform int uLightCount;
uniform vec4 uLightPositionKind[MaxLights];
uniform vec4 uLightDirectionRange[MaxLights];
uniform vec4 uLightColorIntensity[MaxLights];
uniform vec2 uLightCone[MaxLights];
uniform sampler2D uShadowMap;
uniform int uShadowEnabled;
uniform int uShadowLightIndex;
uniform int uShadowCascadeCount;
uniform mat4 uShadowViewProjection[4];
uniform vec4 uCascadeSplits[4];
uniform vec4 uAtlasRects[4];
uniform vec3 uCameraForward;
uniform vec2 uShadowTexel;
uniform vec4 uShadowBiasFilter;
uniform vec3 uSelectionColor;
uniform float uSelectionStrength;
uniform vec3 uBaseColor;
float SampleCascade(int cascade, vec3 worldPosition, vec3 normal, vec3 lightDirection)
{
    vec3 localized = worldPosition - uCameraPosition + normal * uShadowBiasFilter.z;
    vec4 clip = uShadowViewProjection[cascade] * vec4(localized, 1.0);
    vec3 coordinate = clip.xyz / clip.w * vec4(0.5).xyz + vec3(0.5, 0.5, vec4(0.5).w);
    if (any(lessThanEqual(coordinate, vec3(0.0))) || any(greaterThanEqual(coordinate, vec3(1.0)))) return 1.0;
    vec4 rect = uAtlasRects[cascade];
    vec2 uv = coordinate.xy * rect.xy + rect.zw;
    vec2 texel = vec2(uShadowTexel.x, uShadowTexel.y);
    vec2 minimum = rect.zw + texel * 0.5;
    vec2 maximum = rect.zw + rect.xy - texel * 0.5;
    float bias = uShadowBiasFilter.x + uShadowBiasFilter.y * (1.0 - clamp(dot(normal, lightDirection), 0.0, 1.0));
    int radius = int(uShadowBiasFilter.w);
    float visibility = 0.0;
    float taps = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
            if (abs(x) <= radius && abs(y) <= radius)
            {
                vec2 sampleUv = clamp(uv + vec2(x, y) * texel, minimum, maximum);
                float closest = texture(uShadowMap, sampleUv).r;
                visibility += coordinate.z - bias <= closest ? 1.0 : 0.0;
                taps += 1.0;
            }
    return visibility / max(taps, 1.0);
}
float DirectionalShadow(vec3 worldPosition, vec3 normal, vec3 lightDirection)
{
    if (uShadowEnabled == 0) return 1.0;
    float distance = dot(worldPosition - uCameraPosition, uCameraForward);
    int count = uShadowCascadeCount;
    int cascade = count;
    for (int index = 0; index < count; ++index)
        if (distance <= uCascadeSplits[index].x) { cascade = index; break; }
    if (cascade >= count) return 1.0;
    if (distance < uCascadeSplits[0].z) return 1.0;
    float visibility = SampleCascade(cascade, worldPosition, normal, lightDirection);
    float blend = clamp((distance - uCascadeSplits[cascade].y) / max(uCascadeSplits[cascade].x - uCascadeSplits[cascade].y, 0.0001), 0.0, 1.0);
    if (cascade + 1 < count && blend > 0.0)
        visibility = mix(visibility, SampleCascade(cascade + 1, worldPosition, normal, lightDirection), blend);
    return visibility;
}

void main()
{
    vec3 normal = normalize(vWorldNormal);
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    vec3 baseColor = uBaseColor;
    vec3 lighting = baseColor * 0.08;
    for (int index = 0; index < uLightCount; ++index)
    {
        int kind = int(uLightPositionKind[index].w + 0.5);
        vec3 lightDirection;
        float attenuation = 1.0;
        if (kind == 0)
        {
            lightDirection = normalize(-uLightDirectionRange[index].xyz);
        }
        else
        {
            vec3 toLight = uLightPositionKind[index].xyz - vWorldPosition;
            float distanceToLight = length(toLight);
            lightDirection = distanceToLight > 0.0001 ? toLight / distanceToLight : normal;
            float range = max(uLightDirectionRange[index].w, 0.0001);
            float normalizedDistance = distanceToLight / range;
            float rangeFade = max(1.0 - normalizedDistance * normalizedDistance, 0.0);
            attenuation = rangeFade * rangeFade;
            if (kind == 2)
            {
                float coneCosine = dot(normalize(uLightDirectionRange[index].xyz), -lightDirection);
                attenuation *= smoothstep(uLightCone[index].y, uLightCone[index].x, coneCosine);
            }
        }
        float diffuse = max(dot(normal, lightDirection), 0.0);
        vec3 halfDirection = normalize(lightDirection + viewDirection);
        float specular = pow(max(dot(normal, halfDirection), 0.0), 48.0) * 0.18;
        vec3 radiance = uLightColorIntensity[index].rgb * uLightColorIntensity[index].a * attenuation;
        float visibility = index == uShadowLightIndex ? DirectionalShadow(vWorldPosition, normal, lightDirection) : 1.0;
        lighting += radiance * (baseColor * diffuse + specular) * visibility;
    }
    vec3 mapped = lighting / (lighting + vec3(1.0));
    vec3 displayColor = pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));
    outColor = vec4(mix(displayColor, uSelectionColor, uSelectionStrength), 1.0);
}
)glsl";

    inline constexpr char ShadowVertexShader[] = R"glsl(#version 150 core
in vec3 aPosition;
uniform mat4 uShadowMvp;
void main()
{
    gl_Position = uShadowMvp * vec4(aPosition, 1.0);
}
)glsl";

    inline constexpr char ShadowFragmentShader[] = R"glsl(#version 150 core
void main() {}
)glsl";
}  // namespace Horo::Editor::Detail
