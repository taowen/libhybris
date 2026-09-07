#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <GL/glext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define E(name)                                                                \
  __typeof__(&name) p_##name = dlsym(egl, #name);                              \
  if (!p_##name)                                                               \
  return 2
#define G(type, name)                                                          \
  type name = (type)p_eglGetProcAddress(#name);                                \
  if (!name)                                                                   \
  return 2
int attribute_draw(PFNEGLGETPROCADDRESSPROC lookup);
int procedural_draw(PFNEGLGETPROCADDRESSPROC lookup);
int vertex_prepass(PFNEGLGETPROCADDRESSPROC lookup);
int packed_draw(PFNEGLGETPROCADDRESSPROC lookup);
static int desktop_draw(PFNEGLGETPROCADDRESSPROC p_eglGetProcAddress);
int glx_probe(const char *profile, int (*draw)(PFNEGLGETPROCADDRESSPROC));
static void maps(void) {
  FILE *in = fopen("/proc/self/maps", "r"), *out = fopen("maps.txt", "w");
  if (in && out) {
    char line[4096];
    while (fgets(line, sizeof(line), in))
      fputs(line, out);
  }
  if (in)
    fclose(in);
  if (out)
    fclose(out);
}
int main(int argc, char **argv) {
  const char *profile = argc > 1 ? argv[1] : "core32";
  int compat = !strcmp(profile, "compat32"),
      minor = !strcmp(profile, "core33") ? 3 : 2;
  printf("REQUEST_GL 3.%d %s\n", minor, compat ? "compat" : "core");
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(45);
  if (getenv("HYBRIS_GLX_PROBE"))
    return glx_probe(profile, desktop_draw);
  void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!egl) {
    puts(dlerror());
    return 2;
  }
  E(eglGetProcAddress);
  E(eglGetDisplay);
  E(eglInitialize);
  E(eglQueryString);
  E(eglBindAPI);
  E(eglChooseConfig);
  E(eglCreateContext);
  E(eglCreatePbufferSurface);
  E(eglMakeCurrent);
  E(eglGetError);
  E(eglDestroyContext);
  E(eglDestroySurface);
  E(eglTerminate);
  EGLDisplay d = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!p_eglInitialize(d, NULL, NULL)) {
    printf("EGL_INIT_FAIL 0x%x\n", p_eglGetError());
    maps();
    return 2;
  }
  printf("EGL vendor=%s apis=%s\n", p_eglQueryString(d, EGL_VENDOR),
         p_eglQueryString(d, EGL_CLIENT_APIS));
  if (!p_eglBindAPI(EGL_OPENGL_API))
    return 2;
  EGLint attrs[] = {EGL_SURFACE_TYPE,
                    EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE,
                    EGL_OPENGL_BIT,
                    EGL_RED_SIZE,
                    8,
                    EGL_GREEN_SIZE,
                    8,
                    EGL_BLUE_SIZE,
                    8,
                    EGL_ALPHA_SIZE,
                    8,
                    EGL_NONE};
  EGLConfig config;
  EGLint count = 0;
  if (!p_eglChooseConfig(d, attrs, &config, 1, &count) || count != 1)
    return 2;
  EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION,
                 3,
                 EGL_CONTEXT_MINOR_VERSION,
                 minor,
                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                 compat ? EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT
                        : EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                 EGL_NONE};
  EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  EGLContext c = p_eglCreateContext(d, config, EGL_NO_CONTEXT, ca);
  if (c == EGL_NO_CONTEXT) {
    EGLint error = p_eglGetError();
    printf("GL_CONTEXT_REJECTED 0x%x\n", error);
    maps();
    return error == EGL_BAD_MATCH ? 3 : 2;
  }
  EGLSurface s = p_eglCreatePbufferSurface(d, config, pa);
  if (s == EGL_NO_SURFACE) {
    printf("PBUFFER_REJECTED 0x%x\n", p_eglGetError());
    maps();
    return 2;
  }
  if (c == EGL_NO_CONTEXT || s == EGL_NO_SURFACE ||
      !p_eglMakeCurrent(d, s, s, c)) {
    printf("GL_CONTEXT_FAIL 0x%x\n", p_eglGetError());
    maps();
    return 2;
  }
  int result = desktop_draw(p_eglGetProcAddress);
  p_eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  p_eglDestroySurface(d, s);
  p_eglDestroyContext(d, c);
  p_eglTerminate(d);
  return result;
}

static int desktop_draw(PFNEGLGETPROCADDRESSPROC p_eglGetProcAddress) {
  G(PFNGLGETSTRINGIPROC, glGetStringi);
  G(PFNGLGETINTEGERVPROC, glGetIntegerv);
  G(PFNGLGETSTRINGPROC, glGetString);
  G(PFNGLGETERRORPROC, glGetError);
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
  G(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
  G(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
  G(PFNGLDRAWARRAYSPROC, glDrawArrays);
  G(PFNGLVIEWPORTPROC, glViewport);
  G(PFNGLREADPIXELSPROC, glReadPixels);
  G(PFNGLDELETESHADERPROC, glDeleteShader);
  G(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
  G(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  printf("GL vendor=%s renderer=%s version=%s GLSL=%s\n",
         glGetString(GL_VENDOR), renderer, glGetString(GL_VERSION),
         glGetString(GL_SHADING_LANGUAGE_VERSION));
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  if (major > 4 || (major == 4 && minor >= 3)) {
    GLint vertex = -1, fragment = -1, compute = -1;
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vertex);
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fragment);
    glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &compute);
    printf("SSBO_LIMITS vertex=%d fragment=%d compute=%d error=0x%x\n",
           vertex, fragment, compute, glGetError());
  }
  GLint extension_count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
  for (GLint i = 0; i < extension_count; i++)
    printf("GL_EXTENSION %s\n", glGetStringi(GL_EXTENSIONS, (GLuint)i));
  if (!renderer || !strstr(renderer, "zink")) {
    puts("NOT_ZINK");
    maps();
    return 2;
  }
  const char *sources[] = {
      "#version 150 core\nconst vec2 "
      "p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));void "
      "main(){gl_Position=vec4(p[gl_VertexID],0,1);}",
      "#version 150 core\nout vec4 color;void "
      "main(){color=gl_FragCoord.x<8?vec4(1,0,0,1):vec4(0,1,0,1);}"};
  GLuint program = glCreateProgram(), shaders[2];
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
      maps();
      return 2;
    }
    glAttachShader(program, shaders[i]);
  }
  glLinkProgram(program);
  GLint linked;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[4096];
    glGetProgramInfoLog(program, sizeof(log), NULL, log);
    puts(log);
    maps();
    return 2;
  }
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glUseProgram(program);
  glViewport(0, 0, 16, 16);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  unsigned char pixels[16 * 16 * 4];
  memset(pixels, 0, sizeof(pixels));
  glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  GLenum error = glGetError();
  int bad = 0;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      unsigned char *v = pixels + (y * 16 + x) * 4;
      if (v[0] != (x < 8 ? 255 : 0) || v[1] != (x < 8 ? 0 : 255) || v[2] ||
          v[3] != 255)
        bad++;
    }
  FILE *f = fopen("image.rgba", "wb");
  if (!f)
    return 2;
  fwrite(pixels, 1, sizeof(pixels), f);
  fclose(f);
  maps();
  int packed = packed_draw(p_eglGetProcAddress);
  if (packed)
    bad++;
  if (getenv("HYBRIS_VERTEX_PREPASS") && vertex_prepass(p_eglGetProcAddress))
    bad++;
  if (getenv("HYBRIS_PROCEDURAL_VERTEX") && attribute_draw(p_eglGetProcAddress))
    return 2;
  if (getenv("HYBRIS_PROCEDURAL_VERTEX") && procedural_draw(p_eglGetProcAddress))
    bad++;
  glDeleteVertexArrays(1, &vao);
  glDeleteProgram(program);
  for (int i = 0; i < 2; i++)
    glDeleteShader(shaders[i]);
  printf("DESKTOP_GL_DRAW %s bad_pixels=%d error=0x%x\n",
         !bad && !error ? "PASS" : "FAIL", bad, error);
  return bad || error ? 2 : 0;
}
