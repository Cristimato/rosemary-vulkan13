#version 450
layout(location=0) in vec4 pos;
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[1]; float gl_CullDistance[1]; };
void main(){ gl_Position = pos; gl_ClipDistance[0] = pos.x; gl_CullDistance[0] = pos.y; }
