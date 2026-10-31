#version 450
#extension GL_EXT_blend_func_extended : require
layout(location=0,index=0) out vec4 first;
layout(location=0,index=1) out vec4 second;
layout(location=1) out vec4 auxiliary;
void main(){first=vec4(0.25,0.5,0.75,0.5); second=vec4(0.75,0.25,0.5,0.25); auxiliary=vec4(1);}
