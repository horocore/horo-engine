#version 410

layout(std140) uniform type_VfxInstances
{
    mat4 ClipFromLocal[128];
    vec4 InstanceColor[128];
} VfxInstances;

layout(std140) uniform type_VfxMaterial
{
    vec4 Tint;
    float AlphaCutoff;
} VfxMaterial;

layout(location = 0) in vec3 in_var_POSITION;
layout(location = 2) in vec2 in_var_TEXCOORD0;
layout(location = 0) out vec2 horo_vfx_uv;
layout(location = 1) out vec4 horo_vfx_color;

void main()
{
    precise vec4 _14 = VfxInstances.ClipFromLocal[uint(gl_InstanceID)] * vec4(in_var_POSITION, 1.0);
    precise vec4 _15 = VfxInstances.InstanceColor[uint(gl_InstanceID)] * VfxMaterial.Tint;
    gl_Position = _14;
    horo_vfx_uv = in_var_TEXCOORD0;
    horo_vfx_color = _15;
}

