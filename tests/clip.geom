#version 450
layout(triangles) in; layout(triangle_strip, max_vertices=3) out;
void main(){ for(int i=0;i<3;i++){ gl_Position = gl_in[i].gl_Position; gl_ClipDistance[0] = gl_in[i].gl_ClipDistance[0]; EmitVertex(); } EndPrimitive(); }
