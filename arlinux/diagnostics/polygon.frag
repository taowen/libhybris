#version 450
layout(push_constant) uniform P {vec4 color;} p;
layout(location=0) out vec4 color;
void main(){color=p.color;}
