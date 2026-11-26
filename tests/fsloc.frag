#version 450
layout(location = 0) in vec2 uv;
layout(location = 4) in vec4 extra[2];
layout(location = 0) out vec4 o;
void main() { if (uv.x > 2.0) discard; o = vec4(uv, 0, 1) + extra[1]; }
