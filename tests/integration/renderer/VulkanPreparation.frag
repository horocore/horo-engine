#version 460
layout(set = 0, binding = 0, std140) uniform Material { vec4 tint; } material;
layout(set = 0, binding = 1) uniform texture2D albedo;
layout(set = 0, binding = 2) uniform sampler albedoSampler;
layout(constant_id = 7) const uint gain = 42;
layout(location = 0) out vec4 color;
void main() {
    color = material.tint * texture(sampler2D(albedo, albedoSampler), vec2(0.5)) * float(gain);
}
