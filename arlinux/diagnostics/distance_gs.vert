#version 450
out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[2]; };
void main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
gl_Position=vec4(p[gl_VertexIndex],0,1);gl_ClipDistance[0]=gl_Position.x;gl_ClipDistance[1]=1;}
