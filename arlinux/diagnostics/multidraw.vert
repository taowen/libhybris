#version 450
layout(location=0) flat out int instance;
void main(){
    vec2 p[6]=vec2[](vec2(-1,-1),vec2(1,-1),vec2(-1,1),vec2(-1,1),vec2(1,-1),vec2(1,1));
    int column=gl_InstanceIndex;
    gl_Position=vec4((p[gl_VertexIndex-6].x+1.0)/3.0-1.0+float(column)*2.0/3.0,p[gl_VertexIndex-6].y,0,1);
    instance=column;
}
