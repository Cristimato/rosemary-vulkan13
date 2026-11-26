#version 450
layout(location = 0) flat in int id;
layout(location = 0) out vec4 o;
void main() { o = vec4(float(id + 1) / 8.0, 0.0, 0.0, 1.0); }
