#version 410

uniform sampler2D SPIRV_Cross_CombinedBaseColorBaseColorSampler;

layout(location = 0) in vec2 horo_vfx_uv;
layout(location = 1) in vec4 horo_vfx_color;
layout(location = 0) out vec4 out_var_SV_Target0;

void main()
{
    precise vec4 _10 = texture(SPIRV_Cross_CombinedBaseColorBaseColorSampler, horo_vfx_uv) * horo_vfx_color;
    out_var_SV_Target0 = _10;
}

