#version 450
#extension GL_EXT_control_flow_attributes : enable
// Hand translation of DWM's blur fragment shader as emitted by DXVK (tools/repro-shaders/fs.dis):
// 6 iterations x 2 texture samples with bias, mirror-repeat addressing emulated in the shader,
// weights and offsets read as scalars from a uniform buffer indexed by the loop counter.
layout(set = 0, binding = 0) uniform sampler smp[4];
layout(set = 1, binding = 0) uniform texture2D tex;
layout(set = 2, binding = 2) uniform CB { vec4 c[20]; } cb;
layout(push_constant) uniform PC { uint sidx; } pc;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 col;

float mirror_axis(float v, float lo, float len, float len2)
{
    float t = v - lo;
    float q = t / len2;
    float fl = floor(q);
    float m = fma(-len2, fl, t);
    float ge = (m >= len) ? 1.0 : 0.0;
    float refl = fma(m, -2.0, len2);
    return fma(ge, refl, m) + lo;
}

void main()
{
    vec4 a = cb.c[0];
    float f66 = a.z - a.x;
    float f67 = a.w - a.y;
    float f68 = f66 + f66;
    float f69 = f67 + f67;
    float acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, acc3 = 0.0;
    [[dont_unroll]]
    for (int i = 0; i < 6; i++) {
        int idx = i + 5;
        float c0 = cb.c[idx].x;
        float c2 = cb.c[idx].z;
        vec4 s = cb.c[19];
        float f106 = fma(c0, s.x, uv.x);
        float f107 = fma(c0, s.y, uv.y);
        float f108 = fma(c2, s.x, uv.x);
        float f109 = fma(c2, s.y, uv.y);
        vec3 r2 = cb.c[2].xyz;
        vec3 r3 = cb.c[3].xyz;
        vec3 p1 = vec3(f106, f107, 1.0);
        vec3 p2 = vec3(f108, f109, 1.0);
        vec4 b = cb.c[1];
        vec2 t1 = vec2(clamp(mirror_axis(dot(r2, p1), cb.c[0].x, f66, f68), b.x, b.z),
                       clamp(mirror_axis(dot(r3, p1), cb.c[0].y, f67, f69), b.y, b.w));
        vec4 s1 = texture(sampler2D(tex, smp[pc.sidx & 0xffffu]), t1, -0.475);
        float w1 = cb.c[idx].y;
        acc0 = fma(w1, s1.x, acc0); acc1 = fma(w1, s1.y, acc1); acc2 = fma(w1, s1.z, acc2); acc3 = fma(w1, s1.w, acc3);
        vec2 t2 = vec2(clamp(mirror_axis(dot(r2, p2), cb.c[0].x, f66, f68), b.x, b.z),
                       clamp(mirror_axis(dot(r3, p2), cb.c[0].y, f67, f69), b.y, b.w));
        vec4 s2 = texture(sampler2D(tex, smp[pc.sidx & 0xffffu]), t2, -0.475);
        float w2 = cb.c[idx].w;
        acc0 = fma(w2, s2.x, acc0); acc1 = fma(w2, s2.y, acc1); acc2 = fma(w2, s2.z, acc2); acc3 = fma(w2, s2.w, acc3);
    }
    col = vec4(acc0, acc1, acc2, acc3);
}
