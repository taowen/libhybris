#include <EGL/egl.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int indexed_draw(PFNEGLGETPROCADDRESSPROC lookup) {
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
  G(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
  G(PFNGLUNIFORM1IPROC, glUniform1i);
  G(PFNGLGENBUFFERSPROC, glGenBuffers);
  G(PFNGLBINDBUFFERPROC, glBindBuffer);
  G(PFNGLBUFFERDATAPROC, glBufferData);
  G(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData);
  G(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
  G(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
  G(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
  G(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
  G(PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC,
    glDrawElementsInstancedBaseVertexBaseInstance);
  G(PFNGLPRIMITIVERESTARTINDEXPROC, glPrimitiveRestartIndex);
  G(PFNGLENABLEPROC, glEnable);
  G(PFNGLDISABLEPROC, glDisable);
  G(PFNGLVIEWPORTPROC, glViewport);
  G(PFNGLCLEARCOLORPROC, glClearColor);
  G(PFNGLCLEARPROC, glClear);
  G(PFNGLREADPIXELSPROC, glReadPixels);
  G(PFNGLGETERRORPROC, glGetError);
  G(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
  G(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
  G(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
  G(PFNGLDELETESHADERPROC, glDeleteShader);
  const char *source[2] = {
      "#version 430 core\n#extension GL_ARB_shader_draw_parameters : require\n"
      "layout(location=0) in vec2 position;uniform int expected_base;"
      "flat out int valid;void main(){gl_Position=vec4(position,0,1);"
      "valid=int(gl_VertexID>=4&&gl_VertexID<=7&&gl_BaseVertexARB==expected_"
      "base"
      "&&gl_BaseInstanceARB==3&&gl_InstanceID==0);}",
      "#version 430 core\nflat in int valid;out vec4 color;void main(){"
      "int diagonal=int(gl_FragCoord.x)+int(gl_FragCoord.y);"
      "color=valid==0?vec4(1,0,1,1):diagonal==15?vec4(0,0,1,1):"
      "gl_PrimitiveID==0?vec4(1,0,0,1):gl_PrimitiveID==1?vec4(0,1,0,1):vec4(1,"
      "0,1,1);}"};
  GLuint program = glCreateProgram(), shaders[2];
  for (int i = 0; i < 2; i++) {
    shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    glShaderSource(shaders[i], 1, &source[i], NULL);
    glCompileShader(shaders[i]);
    GLint ok;
    glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[4096];
      glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
      puts(log);
      return 2;
    }
    glAttachShader(program, shaders[i]);
  }
  glLinkProgram(program);
  GLint ok;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    puts(log);
    return 2;
  }
  glUseProgram(program);
  GLuint vao, buffers[3];
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(3, buffers);
  float positions[8][2] = {{0},      {0},     {0},     {0},
                           {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, NULL);
  glEnableVertexAttribArray(0);
  int failures = 0;
  for (int phase = 0; phase < 9; phase++) {
    unsigned width = phase == 8 ? 1 : 1u << (phase % 3);
    GLenum type = width == 1   ? GL_UNSIGNED_BYTE
                  : width == 2 ? GL_UNSIGNED_SHORT
                               : GL_UNSIGNED_INT;
    int base = width == 2 ? -2 : width == 4 ? 1 : 4;
    int restart = phase < 3 || phase == 6 || phase == 7;
    unsigned marker = width == 1   ? 255
                      : width == 2 ? (phase == 7 ? 0x1234 : 0xffff)
                                   : UINT32_MAX;
    unsigned order[7] = {0, 1, 2, UINT32_MAX, 2, 1, 3};
    if (!restart) {
      order[3] = 2;
      order[4] = 1;
      order[5] = 3;
    }
    unsigned count = phase == 8 ? 3 : restart ? 7 : 6;
    unsigned prefix = phase == 8 ? 0 : 2;
    unsigned char indices[36];
    memset(indices, 0xa5, sizeof(indices));
    for (unsigned i = 0; i < count; i++) {
      uint32_t value =
          phase == 6 || order[i] == UINT32_MAX ? marker : order[i] + 4 - base;
      memcpy(indices + (i + prefix) * width, &value, width);
    }
    /* GPU copy produces the actual EBO; exact lengths expose partial tails. */
    glBindBuffer(GL_COPY_READ_BUFFER, buffers[1]);
    glBufferData(GL_COPY_READ_BUFFER, (count + prefix) * width, indices,
                 GL_STREAM_DRAW);
    /* A fresh object prevents allocation reuse from hiding a short tail. */
    if (phase) {
      glDeleteBuffers(1, &buffers[2]);
      glGenBuffers(1, &buffers[2]);
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (count + prefix) * width, NULL,
                 GL_STREAM_DRAW);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_ELEMENT_ARRAY_BUFFER, 0, 0,
                        (count + prefix) * width);
    if (restart)
      glEnable(GL_PRIMITIVE_RESTART);
    else
      glDisable(GL_PRIMITIVE_RESTART);
    glPrimitiveRestartIndex(marker);
    glUniform1i(glGetUniformLocation(program, "expected_base"), base);
    glViewport(0, 0, 16, 16);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElementsInstancedBaseVertexBaseInstance(
        restart ? GL_TRIANGLE_STRIP : GL_TRIANGLES, count, type,
        (void *)(uintptr_t)(prefix * width), 1, base, 3);
    unsigned char pixels[1024];
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    int bad = 0;
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x++) {
        unsigned char expected[4] = {0, 0, 0, 255};
        expected[phase == 6 || (phase == 8 && x + y > 15) || x + y == 15 ? 2
                 : x + y < 15 ? 0
                              : 1] = 255;
        bad += memcmp(pixels + (y * 16 + x) * 4, expected, 4) != 0;
      }
    char name[64];
    snprintf(name, sizeof(name), "indexed-%d.rgba", phase);
    FILE *f = fopen(name, "wb");
    if (!f)
      return 2;
    size_t written = fwrite(pixels, 1, sizeof(pixels), f);
    int closed = fclose(f);
    GLenum error = glGetError();
    int failed = bad || error || written != sizeof(pixels) || closed;
    printf("INDEXED_VERTEX phase=%d %s bad_pixels=%d error=0x%x\n", phase,
           failed ? "FAIL" : "PASS", bad, error);
    failures += failed;
  }
  glDisable(GL_PRIMITIVE_RESTART);
  glDeleteBuffers(3, buffers);
  glBindVertexArray(0);
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(program);
  for (int i = 0; i < 2; i++)
    glDeleteShader(shaders[i]);
  return failures ? 2 : 0;
}
