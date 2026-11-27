#version 450
layout(location = 0) flat in int did;
layout(location = 0) out vec4 o;
void main() { o = vec4(float(did + 1) * 40.0 / 255.0, 1.0, 0.0, 1.0); }
