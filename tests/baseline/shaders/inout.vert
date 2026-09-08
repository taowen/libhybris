#version 450
layout(location = 0) out vec4 vColor;
void main()
{
    vColor = vec4(1.0, 0.25, 0.0, 1.0);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
