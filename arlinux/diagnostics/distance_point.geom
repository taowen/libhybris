#version 450
layout(triangles) in;
layout(triangle_strip,max_vertices=3) out;
layout(location=0) in float occupied_in[][8];
layout(location=0) out float occupied[8];
in gl_PerVertex {vec4 gl_Position;float gl_ClipDistance[1];float gl_CullDistance[1];}gl_in[];
out gl_PerVertex {vec4 gl_Position;float gl_PointSize;float gl_ClipDistance[1];float gl_CullDistance[1];};
void main(){for(int i=0;i<3;++i){gl_Position=gl_in[i].gl_Position;
gl_ClipDistance[0]=gl_in[i].gl_ClipDistance[0];gl_CullDistance[0]=gl_in[i].gl_CullDistance[0];gl_PointSize=1;
for(int j=0;j<8;++j)occupied[j]=occupied_in[i][j];EmitVertex();}EndPrimitive();}
