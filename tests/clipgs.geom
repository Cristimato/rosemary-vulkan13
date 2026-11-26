#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) out vec2 uv;
void main() {
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        gl_ClipDistance[0] = gl_in[i].gl_Position.x;
        gl_ClipDistance[1] = 1.0; gl_ClipDistance[2] = 1.0; gl_ClipDistance[3] = 1.0; gl_ClipDistance[4] = gl_in[i].gl_Position.y;
        uv = gl_in[i].gl_Position.xy;
        EmitVertex();
    }
    EndPrimitive();
}
