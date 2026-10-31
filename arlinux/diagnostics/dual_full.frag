#version 450
#extension GL_EXT_blend_func_extended : require
layout(location=0,index=0) out vec4 first;
layout(location=0,index=1) out vec4 second;
layout(location=1) out vec4 auxiliary1;
layout(location=2) out vec4 auxiliary2;
layout(location=3) out vec4 auxiliary3;
layout(location=4) out vec4 auxiliary4;
layout(location=5) out vec4 auxiliary5;
layout(location=6) out vec4 auxiliary6;
layout(location=7) out vec4 auxiliary7;
void main(){first=vec4(0.25,0.5,0.75,0.5); second=vec4(0.75,0.25,0.5,0.25);auxiliary1=vec4(1);auxiliary2=vec4(1);auxiliary3=vec4(1);auxiliary4=vec4(1);auxiliary5=vec4(1);auxiliary6=vec4(1);auxiliary7=vec4(1);}
