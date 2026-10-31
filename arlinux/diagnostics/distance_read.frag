#version 450
layout(location=0) in float occupied[8];
layout(location=0) out vec4 color;
in float gl_ClipDistance[1];
void main(){float red=0;for(int i=0;i<8;++i)red+=occupied[i];color=vec4(red*(gl_ClipDistance[0]>=0?1:0),0,0,1);}
