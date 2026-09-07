#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int resource_draw(PFNEGLGETPROCADDRESSPROC lookup) {
#define G(type, name)                                                          \
  type name = (type)lookup(#name);                                             \
  if (!name)                                                                   \
  return 2
  G(PFNGLCREATESHADERPROC, glCreateShader);
  G(PFNGLSHADERSOURCEPROC, glShaderSource);
  G(PFNGLCOMPILESHADERPROC, glCompileShader);
  G(PFNGLGETSHADERIVPROC, glGetShaderiv);
  G(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
  G(PFNGLCREATEPROGRAMPROC, glCreateProgram);
  G(PFNGLATTACHSHADERPROC, glAttachShader);
  G(PFNGLLINKPROGRAMPROC, glLinkProgram);
  G(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
  G(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
  G(PFNGLUSEPROGRAMPROC, glUseProgram);
  G(PFNGLGENBUFFERSPROC, glGenBuffers);
  G(PFNGLBINDBUFFERPROC, glBindBuffer);
  G(PFNGLBUFFERDATAPROC, glBufferData);
  G(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
  G(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
  G(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
  G(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
  G(PFNGLVIEWPORTPROC, glViewport);
  G(PFNGLCLEARCOLORPROC, glClearColor);
  G(PFNGLCLEARPROC, glClear);
  G(PFNGLREADPIXELSPROC, glReadPixels);
  G(PFNGLGETERRORPROC, glGetError);
  G(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
  G(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
  G(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
  G(PFNGLDELETESHADERPROC, glDeleteShader);
  G(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced);
  G(PFNGLGENTEXTURESPROC, glGenTextures);
  G(PFNGLACTIVETEXTUREPROC, glActiveTexture);
  G(PFNGLBINDTEXTUREPROC, glBindTexture);
  G(PFNGLTEXIMAGE2DPROC, glTexImage2D);
  G(PFNGLTEXPARAMETERIPROC, glTexParameteri);
  G(PFNGLDELETETEXTURESPROC, glDeleteTextures);
  G(PFNGLGETINTEGERVPROC, glGetIntegerv);
  G(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced);
  GLint texture_count = 0;
  glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &texture_count);
  if (texture_count < 16 || texture_count > 32)
    return 2;
  printf("RESOURCE_CAPACITY vertex_samplers=%d\n", texture_count);
  GLuint programs[2], shaders[2][2];
  const char *fragment =
      "#version 430 core\nflat in vec4 color;flat in int instance;out vec4 "
      "result;"
      "void "
      "main(){if((gl_FragCoord.x>=8)!=(instance==1))discard;result=color;}";
  for (int variant = 0; variant < 2; variant++) {
    char vertex[8192];
    size_t used = snprintf(vertex, sizeof(vertex), "#version 430 core\n");
    for (int i = 0; i < (variant ? 1 : 16); i++)
      used += snprintf(vertex + used, sizeof(vertex) - used,
                       "layout(location=%d) in float a%d;", i, i);
    int sampled = variant ? texture_count : texture_count - 16;
    if (sampled)
      used +=
          snprintf(vertex + used, sizeof(vertex) - used,
                   "layout(binding=0) uniform sampler2D images[%d];", sampled);
    used += snprintf(
        vertex + used, sizeof(vertex) - used,
        "flat out vec4 color;flat out int instance;void main(){float sum=0;");
    for (int i = 0; i < (variant ? 1 : 16); i++)
      used += snprintf(vertex + used, sizeof(vertex) - used, "sum+=a%d;", i);
    if (sampled)
      for (int i = 0; i < sampled; i++)
        used += snprintf(vertex + used, sizeof(vertex) - used,
                         "sum+=texelFetch(images[%d],ivec2(0),0).r;", i);
    snprintf(
        vertex + used, sizeof(vertex) - used,
        "vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
        "instance=gl_InstanceID;vec2 "
        "pos=p[gl_VertexID];pos.x=pos.x*.5+(instance==0?-.5:.5);"
        "gl_Position=vec4(pos,0,1);color=sum==%d.0?(instance==0?vec4(1,0,0,"
        "1):vec4(0,1,0,1)):vec4(1,0,1,1);}",
        (variant ? 1 : 136) + sampled);
    programs[variant] = glCreateProgram();
    const char *sources[2] = {vertex, fragment};
    for (int i = 0; i < 2; i++) {
      shaders[variant][i] =
          glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
      glShaderSource(shaders[variant][i], 1, &sources[i], NULL);
      glCompileShader(shaders[variant][i]);
      GLint ok;
      glGetShaderiv(shaders[variant][i], GL_COMPILE_STATUS, &ok);
      if (!ok) {
        char log[4096];
        glGetShaderInfoLog(shaders[variant][i], sizeof(log), NULL, log);
        puts(log);
        return 2;
      }
      glAttachShader(programs[variant], shaders[variant][i]);
    }
    glLinkProgram(programs[variant]);
    GLint ok;
    glGetProgramiv(programs[variant], GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[4096];
      glGetProgramInfoLog(programs[variant], sizeof(log), NULL, log);
      puts(log);
      return 2;
    }
  }
  GLuint vao, buffers[16], texture;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(16, buffers);
  for (int i = 0; i < 16; i++) {
    float values[3] = {i + 1, i + 1, i + 1};
    glBindBuffer(GL_ARRAY_BUFFER, buffers[i]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(values), values, GL_STATIC_DRAW);
    glVertexAttribPointer(i, 1, GL_FLOAT, GL_FALSE, 4, NULL);
    glEnableVertexAttribArray(i);
  }
  glGenTextures(1, &texture);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  unsigned char texel[4] = {255, 0, 0, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               texel);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  for (int i = 1; i < texture_count; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texture);
  }
  GLuint index_buffer;
  glGenBuffers(1, &index_buffer);
  int failures = 0;
  for (int phase = 0; phase < 6; phase++) {
    glUseProgram(programs[phase == 1]);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (phase == 3 || phase == 4) {
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
      const uint32_t wide[3] = {0, 1, 2};
      const unsigned char narrow[3] = {0, 1, 2};
      glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                   phase == 3 ? sizeof(wide) : sizeof(narrow),
                   phase == 3 ? (const void *)wide : (const void *)narrow,
                   GL_STREAM_DRAW);
      glDrawElementsInstanced(GL_TRIANGLES, 3,
                              phase == 3 ? GL_UNSIGNED_INT : GL_UNSIGNED_BYTE,
                              NULL, 2);
      /* Force a new short resource for the next indexed phase. */
      glDeleteBuffers(1, &index_buffer);
      glGenBuffers(1, &index_buffer);
    } else {
      glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
    }
    unsigned char pixels[1024];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int bad = 0;
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++) {
        unsigned char expected[4] = {x < 8 ? 255 : 0, x < 8 ? 0 : 255, 0, 255};
        bad += memcmp(pixels + (y * 16 + x) * 4, expected, 4) != 0;
      }
    char name[64];
    snprintf(name, sizeof(name), "resources-%d.rgba", phase);
    FILE *f = fopen(name, "wb");
    if (!f)
      return 2;
    size_t written = fwrite(pixels, 1, sizeof(pixels), f);
    int closed = fclose(f);
    GLenum error = glGetError();
    int failed = bad || error || written != sizeof(pixels) || closed;
    printf("RESOURCE_VERTEX phase=%d %s bad_pixels=%d error=0x%x\n", phase,
           failed ? "FAIL" : "PASS", bad, error);
    failures += failed;
  }
  glDeleteBuffers(1, &index_buffer);
  glDeleteTextures(1, &texture);
  glActiveTexture(GL_TEXTURE0);
  glDeleteBuffers(16, buffers);
  glBindVertexArray(0);
  glDeleteVertexArrays(1, &vao);
  for (int v = 0; v < 2; v++) {
    glDeleteProgram(programs[v]);
    for (int i = 0; i < 2; i++)
      glDeleteShader(shaders[v][i]);
  }
  return failures ? 2 : 0;
}
