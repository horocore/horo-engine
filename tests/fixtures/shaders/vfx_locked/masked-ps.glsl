#version 410

layout(std140) uniform type_VfxMaterial
{
    vec4 Tint;
    float AlphaCutoff;
} VfxMaterial;

uniform sampler2D SPIRV_Cross_CombinedBaseColorBaseColorSampler;

layout(location = 0) in vec2 horo_vfx_uv;
layout(location = 1) in vec4 horo_vfx_color;
layout(location = 0) out vec4 out_var_SV_Target0;

void main()
{
    precise vec4 _12 = texture(SPIRV_Cross_CombinedBaseColorBaseColorSampler, horo_vfx_uv) * horo_vfx_color;
    precise float _13 = _12.w - VfxMaterial.AlphaCutoff;
    if (_13 < 0.0)
    {
        discard;
    }
    out_var_SV_Target0 = _12;
}

