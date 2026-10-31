#version 450
#extension GL_EXT_blend_func_extended : require
layout(location=0,index=0) out vec4 first;
void main(){first=vec4(0.25,0.5,0.75,0.5);}
