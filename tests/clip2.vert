#version 450
layout(location=0) in vec4 pos;
layout(location=0) out vec4 col;
void main(){ gl_Position = pos; gl_PointSize = 1.0; gl_ClipDistance[0] = dot(pos, vec4(1)); gl_ClipDistance[1] = pos.z; col = pos; }
