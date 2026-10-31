#version 450
layout(location=0) flat in int instance;
layout(location=0) out vec4 color;
void main(){color=vec4(instance==0?1:0,instance==1?1:0,instance==2?1:0,1);}
