#version 450
// carga de varyings: 5 entradas interpoladas e pouca conta
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 v0;
layout(location = 2) in vec4 v1;
layout(location = 3) in vec4 v2;
layout(location = 4) in vec4 v3;
layout(location = 0) out vec4 o;
void main() { o = (v0 * 0.3 + v1 * 0.2 + v2 * 0.2 + v3 * 0.2 + vec4(uv, 0.0, 0.0) * 0.1) * 0.05; }
