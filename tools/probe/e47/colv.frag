#version 450
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(p.col.rgb, 1.0) + vec4(0.0 * uv, 0.0, 0.0); }
