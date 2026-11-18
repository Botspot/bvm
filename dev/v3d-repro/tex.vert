#version 450
// vertex buffer input (pos, uv) like a real draw: adds a vertex buffer BO to the job
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 inuv;
layout(location = 0) out vec2 uv;
void main() {
    uv = inuv;
    gl_Position = vec4(pos, 0.0, 1.0);
}
