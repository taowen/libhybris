#version 450
layout(location=16) in float occupied[16];
layout(location=0) out vec4 color;
void main(){float red=0;for(int i=0;i<16;++i)red+=occupied[i];color=vec4(red,0,0,1);}
