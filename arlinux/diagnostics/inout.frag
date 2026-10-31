#version 450
layout(location=0) in vec2 value;
layout(location=0) out vec4 color;
void main(){color=vec4(value,0,1);}
