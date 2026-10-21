#version 460
layout(set = 0, binding = 0, std140) uniform Material {
    vec4 tint;
    uint viewIndex;
    mat4 worldToClip[2];
} material;
layout(constant_id = 7) const uint gain = 42;
layout(location = 0) out vec4 color;
void main() { color = material.tint * (float(gain) / 13.0); }
