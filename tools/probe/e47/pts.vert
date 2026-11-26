#version 450
layout(push_constant) uniform P { vec4 col; float z; float w; } p;
void main() {
    int i = gl_VertexIndex;
    gl_Position = vec4(-0.75 + 0.5 * float(i & 3), -0.75 + 0.5 * float(i >> 2), 0.5, 1.0);
    gl_PointSize = 4.0;
}
