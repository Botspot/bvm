#version 450
// samples the previous pass's image (blur/downsample chain) + a uniform buffer: adds texture + UBO BOs
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform U { vec4 tint; } u;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    color = texture(tex, uv) * u.tint;
}
