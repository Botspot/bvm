#version 450
#extension GL_EXT_control_flow_attributes : enable
// Reduced version of DWM's blur fragment shader: a loop with two biased texture samples per iteration,
// weights/offsets read from a uniform buffer indexed by the loop counter (README item 122).
layout(set = 0, binding = 0) uniform sampler smp[4];
layout(set = 1, binding = 0) uniform texture2D tex;
layout(set = 2, binding = 2) uniform CB { vec4 c[20]; } cb;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 col;
void main()
{
    vec4 acc = vec4(0.0);
    [[dont_unroll]]
    for (int i = 0; i < 6; i++) {
        vec4 w = cb.c[i + 5];
        vec2 uv0 = clamp(uv * cb.c[19].x * (1.0 / 128.0) + vec2(w.x, w.z) * (1.0 / 128.0), cb.c[0].xy, cb.c[0].zw);
        vec2 uv1 = clamp(uv * cb.c[19].x * (1.0 / 128.0) - vec2(w.x, w.z) * (1.0 / 128.0), cb.c[0].xy, cb.c[0].zw);
        acc += w.y * texture(sampler2D(tex, smp[0]), uv0, -0.475);
        acc += w.w * texture(sampler2D(tex, smp[0]), uv1, -0.475);
    }
    col = acc;
}
