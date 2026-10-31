#version 450
layout(location=16) out float occupied[16];
void main(){
    vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexIndex],0,1);
    for(int i=0;i<16;++i)occupied[i]=1.0/16.0;
    gl_ClipDistance[0]=gl_Position.x; gl_CullDistance[0]=1;
}
