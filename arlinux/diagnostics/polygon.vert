#version 450
layout(location=0) in vec2 position;
layout(set=0,binding=0) uniform U { vec4 translation; } u;
void main(){gl_Position=vec4(position+u.translation.xy+vec2(0,0.0625*float(gl_InstanceIndex-2)),0,1);gl_PointSize=1;}
