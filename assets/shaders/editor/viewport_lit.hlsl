// Canonical development/cook source. Compile and reflect through the selected pinned Horo target route.
cbuffer ViewportParameters : register(b0, space0)
{
    row_major float4x4 Mvp;
    row_major float4x4 ModelToWorld;
    row_major float4x4 ShadowViewProjection;
    float4 CameraAndLightCount;
    float4 LightPositionKind[16];
    float4 LightDirectionRange[16];
    float4 LightColorIntensity[16];
    float4 LightCone[16];
    float4 SelectionColorAndStrength;
    float4 ShadowSettings;
    // XYZ scales projected coordinates; XY biases are 0.5 and W is the depth bias.
    // GL: (0.5,0.5,0.5,0.5). Metal: (0.5,-0.5,1,0).
    float4 ShadowCoordinates;
};
Texture2D<float> ShadowMap : register(t1, space0);
SamplerState ShadowSampler : register(s2, space0);

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal : TEXCOORD1;
    [[vk::location(2)]] float4 shadowPosition : TEXCOORD2;
};

float3 WorldNormal(float3 normal)
{
    // Row-major HLSL stores the host's column-major matrix bytes without repacking, as in the qualified VFX ABI.
    float3x3 columns = (float3x3)ModelToWorld;
    float3 first = cross(columns[1], columns[2]);
    float3 second = cross(columns[2], columns[0]);
    float3 third = cross(columns[0], columns[1]);
    float determinant = dot(columns[0], first);
    float reciprocal = abs(determinant) > 0.0000001 ? 1.0 / determinant : 1.0;
    return normalize((first * normal.x + second * normal.y + third * normal.z) * reciprocal);
}

VertexOutput VSMain(VertexInput input)
{
    VertexOutput output;
    float4 world = mul(float4(input.position, 1.0), ModelToWorld);
    output.position = mul(float4(input.position, 1.0), Mvp);
    output.worldPosition = world.xyz;
    output.worldNormal = WorldNormal(input.normal);
    output.shadowPosition = mul(world, ShadowViewProjection);
    return output;
}

float DirectionalShadow(float3 normal, float3 lightDirection, float4 shadowPosition)
{
    if (ShadowSettings.x == 0.0)
        return 1.0;
    float3 projected = shadowPosition.xyz / shadowPosition.w;
    float3 coordinate = projected * ShadowCoordinates.xyz + float3(0.5, 0.5, ShadowCoordinates.w);
    if (any(coordinate <= 0.0) || any(coordinate >= 1.0))
        return 1.0;
    uint width, height;
    ShadowMap.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);
    float bias = max(0.0025 * (1.0 - dot(normal, lightDirection)), 0.00045);
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            // Preserve linear depth sampling followed by comparison, rather than hardware comparison filtering.
            float closest = ShadowMap.SampleLevel(ShadowSampler, coordinate.xy + float2(x, y) * texel, 0.0);
            visibility += coordinate.z - bias <= closest ? 1.0 : 0.0;
        }
    return visibility / 9.0;
}

float4 LightDirectionAndAttenuation(uint index, float3 worldPosition, float3 normal)
{
    uint kind = (uint)(LightPositionKind[index].w + 0.5);
    if (kind == 0)
        return float4(normalize(-LightDirectionRange[index].xyz), 1.0);
    float3 toLight = LightPositionKind[index].xyz - worldPosition;
    float distance = length(toLight);
    float3 direction = distance > 0.0001 ? toLight / distance : normal;
    float relativeDistance = distance / max(LightDirectionRange[index].w, 0.0001);
    float fade = max(1.0 - relativeDistance * relativeDistance, 0.0);
    float attenuation = fade * fade;
    if (kind == 2)
    {
        float cone = dot(normalize(LightDirectionRange[index].xyz), -direction);
        attenuation *= smoothstep(LightCone[index].y, LightCone[index].x, cone);
    }
    return float4(direction, attenuation);
}

float3 LightContribution(uint index, VertexOutput input, float3 normal, float3 viewDirection)
{
    float4 light = LightDirectionAndAttenuation(index, input.worldPosition, normal);
    float diffuse = max(dot(normal, light.xyz), 0.0);
    float3 halfDirection = normalize(light.xyz + viewDirection);
    float specular = pow(max(dot(normal, halfDirection), 0.0), 48.0) * 0.18;
    float3 radiance = LightColorIntensity[index].rgb * LightColorIntensity[index].w * light.w;
    float visibility = index == (uint)ShadowSettings.y ? DirectionalShadow(normal, light.xyz, input.shadowPosition) : 1.0;
    return radiance * (float3(0.68, 0.70, 0.74) * diffuse + specular) * visibility;
}

float4 PSMain(VertexOutput input) : SV_Target0
{
    float3 normal = normalize(input.worldNormal);
    float3 viewDirection = normalize(CameraAndLightCount.xyz - input.worldPosition);
    float3 lighting = float3(0.68, 0.70, 0.74) * 0.08;
    for (uint index = 0; index < min((uint)CameraAndLightCount.w, 16u); ++index)
        lighting += LightContribution(index, input, normal, viewDirection);
    float3 mapped = lighting / (lighting + 1.0);
    float3 displayColor = pow(max(mapped, 0.0), 1.0 / 2.2);
    return float4(lerp(displayColor, SelectionColorAndStrength.xyz, SelectionColorAndStrength.w), 1.0);
}
