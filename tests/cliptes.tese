#version 450
layout(triangles) in;
layout(location = 3) out vec4 a[2];
void main() {
    vec4 p = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position;
    gl_Position = p;
    gl_ClipDistance[0] = p.x;
    a[0] = p; a[1] = p;
}
