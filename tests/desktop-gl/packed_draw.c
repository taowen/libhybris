/* Desktop GL packed vertex inputs exercise Mesa's real u_vbuf fallback. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define G(type, name)                                                          \
  type name = (type)lookup(#name);                                             \
  if (!name)                                                                   \
  return 2
int packed_draw(PFNEGLGETPROCADDRESSPROC lookup) {
  G(PFNGLCREATESHADERPROC, glCreateShader);
  G(PFNGLSHADERSOURCEPROC, glShaderSource);
  G(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
  G(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
  G(PFNGLCOMPILESHADERPROC, glCompileShader);
  G(PFNGLGETSHADERIVPROC, glGetShaderiv);
  G(PFNGLCREATEPROGRAMPROC, glCreateProgram);
  G(PFNGLATTACHSHADERPROC, glAttachShader);
  G(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation);
  G(PFNGLLINKPROGRAMPROC, glLinkProgram);
  G(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
  G(PFNGLUSEPROGRAMPROC, glUseProgram);
  G(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
  G(PFNGLUNIFORM4FVPROC, glUniform4fv);
  G(PFNGLUNIFORM1IPROC, glUniform1i);
  G(PFNGLGENBUFFERSPROC, glGenBuffers);
  G(PFNGLBINDBUFFERPROC, glBindBuffer);
  G(PFNGLBUFFERDATAPROC, glBufferData);
  G(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
  G(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
  G(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor);
  G(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced);
  G(PFNGLREADPIXELSPROC, glReadPixels);
  G(PFNGLCLEARCOLORPROC, glClearColor);
  G(PFNGLCLEARPROC, glClear);
  G(PFNGLGETERRORPROC, glGetError);
  G(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
  G(PFNGLDELETESHADERPROC, glDeleteShader);
  G(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
  G(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray);
  const char *vs = "#version 150 core\nin vec4 packed_value;uniform vec4 "
                   "expected;uniform vec4 expected_next;uniform int "
                   "divisor;flat out int valid;flat out int instance;const "
                   "vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));void "
                   "main(){valid=all(lessThan(abs(packed_value-(gl_InstanceID/"
                   "divisor==0?expected:expected_next)),vec4(.00001)))?1:0;"
                   "instance=gl_InstanceID;vec2 "
                   "v=p[gl_VertexID];v.x=v.x*.5+(gl_InstanceID==0?-.5:.5);gl_"
                   "Position=vec4(v,0,1);}";
  const char *fs = "#version 150 core\nflat in int valid;flat in int "
                   "instance;out vec4 color;void "
                   "main(){if((instance==0&&gl_FragCoord.x>=8)||(instance==1&&"
                   "gl_FragCoord.x<8))discard;color=valid==1?(instance==0?vec4("
                   "1,0,0,1):vec4(0,1,0,1)):vec4(1,0,1,1);}";
  GLuint program = glCreateProgram(), shaders[2];
  const char *sources[] = {vs, fs};
  for (int i = 0; i < 2; i++) {
    shaders[i] = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    glShaderSource(shaders[i], 1, &sources[i], NULL);
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
  glBindAttribLocation(program, 0, "packed_value");
  glLinkProgram(program);
  GLint linked;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[4096];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    puts(log);
    return 2;
  }
  glUseProgram(program);
  GLint expected = glGetUniformLocation(program, "expected");
  GLuint buffer;
  glGenBuffers(1, &buffer);
  glBindBuffer(GL_ARRAY_BUFFER, buffer);
  glEnableVertexAttribArray(0);
  int failures = 0;
  for (int sign = 0; sign < 2; sign++)
    for (int norm = 0; norm < 2; norm++)
      for (int bgra = 0; bgra < (norm ? 2 : 1); bgra++)
        for (int divisor = 1; divisor <= 2; divisor++) {
          uint32_t bits = sign
                              ? ((uint32_t)(-511) & 1023) |
                                    (((uint32_t)(-256) & 1023) << 10) |
                                    (127u << 20) | (3u << 30)
                              : 123u | (456u << 10) | (789u << 20) | (2u << 30);
          /* Guard words make offset/stride mistakes observable. Distinct
           * instance records must fetch the selected packed input; divisor 2
           * consumes the first entry twice. */
          uint32_t values[] = {0xdeadbeef, bits, 0xdeadbeef, bits ^ 7u,
                               0xdeadbeef};
          GLfloat value[] = {sign ? -511.f : 123.f, sign ? -256.f : 456.f,
                             sign ? 127.f : 789.f, sign ? -1.f : 2.f};
          if (norm) {
            for (int i = 0; i < 3; i++)
              value[i] /= sign ? 511.f : 1023.f;
            value[3] /= sign ? 1.f : 3.f;
          }
          if (bgra) {
            float tmp = value[0];
            value[0] = value[2];
            value[2] = tmp;
          }
          GLfloat next[4];
          memcpy(next, value, sizeof(next));
          next[bgra ? 2 : 0] =
              (sign ? -506.f : 124.f) / (norm ? (sign ? 511.f : 1023.f) : 1.f);
          glUniform4fv(glGetUniformLocation(program, "expected_next"), 1, next);
          glUniform1i(glGetUniformLocation(program, "divisor"), divisor);
          glUniform4fv(expected, 1, value);
          glBufferData(GL_ARRAY_BUFFER, sizeof(values), values, GL_STREAM_DRAW);
          glVertexAttribPointer(0, bgra ? GL_BGRA : 4,
                                sign ? GL_INT_2_10_10_10_REV
                                     : GL_UNSIGNED_INT_2_10_10_10_REV,
                                norm, 8, (void *)4);
          glVertexAttribDivisor(0, divisor);
          glClearColor(0, 0, 1, 1);
          glClear(GL_COLOR_BUFFER_BIT);
          glDrawArraysInstanced(GL_TRIANGLES, 0, 3, 2);
          unsigned char pixels[1024];
          glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
          GLenum error = glGetError();
          char filename[64];
          snprintf(filename, sizeof(filename), "packed-%d-%d-%d-%d.rgba",
                   sign, norm, bgra, divisor);
          FILE *image = fopen(filename, "wb");
          int saved = 0;
          if (image) {
            saved = fwrite(pixels, 1, sizeof(pixels), image) == sizeof(pixels);
            if (fclose(image))
              saved = 0;
          }
          if (!saved) {
            fprintf(stderr, "Failed to save %s\n", filename);
            failures++;
          }

          int bad = 0;
          for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
              unsigned char *v = pixels + (y * 16 + x) * 4;
              if (v[0] != (x < 8 ? 255 : 0) || v[1] != (x < 8 ? 0 : 255) ||
                  v[2] || v[3] != 255)
                bad++;
            }
          printf("PACKED_DRAW signed=%d normalized=%d bgra=%d divisor=%d "
                 "bad=%d error=0x%x image=%s\n",
                 sign, norm, bgra, divisor, bad, error, filename);
          failures += bad || error;
        }
  glVertexAttribDivisor(0, 0);
  glDisableVertexAttribArray(0);
  glDeleteBuffers(1, &buffer);
  glDeleteProgram(program);
  for (int i = 0; i < 2; i++)
    glDeleteShader(shaders[i]);
  return failures ? 2 : 0;
}
