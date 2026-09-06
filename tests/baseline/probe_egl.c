#include "probe.h"

int eglprobe(int version) {
  void *e = dlopen(getenv("PROBE_EGL") ?: "libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!e) {
    printf("EGL dlopen: %s\n", dlerror());
    return 2;
  }
  E(eglGetDisplay);
  E(eglInitialize);
  E(eglQueryString);
  E(eglGetError);
  E(eglChooseConfig);
  E(eglBindAPI);
  E(eglCreateContext);
  E(eglCreatePbufferSurface);
  E(eglMakeCurrent);
  E(eglDestroyContext);
  E(eglDestroySurface);
  E(eglTerminate);
  EGLDisplay d = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major, minor;
  if (!p_eglInitialize(d, &major, &minor)) {
    printf("eglInitialize FAIL 0x%x\n", p_eglGetError());
    return 2;
  }
  printf("EGL %d.%d vendor=%s apis=%s\nextensions=%s\n", major, minor,
         p_eglQueryString(d, EGL_VENDOR), p_eglQueryString(d, EGL_CLIENT_APIS),
         p_eglQueryString(d, EGL_EXTENSIONS));
  int desktop = version == 0;
  const char *apis = p_eglQueryString(d, EGL_CLIENT_APIS);
  if (desktop && (!apis || !strstr(apis, "OpenGL ")) &&
      (!apis || strcmp(apis, "OpenGL"))) {
    printf("DESKTOP_GL UNSUPPORTED (EGL_CLIENT_APIS)\n");
    p_eglTerminate(d);
    return 3;
  }
  EGLint attrs[] = {EGL_SURFACE_TYPE,
                    EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE,
                    desktop ? EGL_OPENGL_BIT
                            : (version == 3 ? 0x40 : EGL_OPENGL_ES2_BIT),
                    EGL_RED_SIZE,
                    8,
                    EGL_GREEN_SIZE,
                    8,
                    EGL_BLUE_SIZE,
                    8,
                    EGL_NONE};
  EGLConfig config;
  EGLint count = 0;
  if (!p_eglChooseConfig(d, attrs, &config, 1, &count)) {
    printf("eglChooseConfig FAIL 0x%x\n", p_eglGetError());
    return 2;
  }
  if (!count) {
    printf("NO CONFIG for %s%d error=0x%x\n", desktop ? "GL" : "GLES", version,
           p_eglGetError());
    return 3;
  }
  if (!p_eglBindAPI(desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API)) {
    printf("eglBindAPI FAIL 0x%x\n", p_eglGetError());
    return 2;
  }
  EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
  EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
  EGLContext c =
      p_eglCreateContext(d, config, EGL_NO_CONTEXT, desktop ? NULL : ca);
  EGLSurface s = p_eglCreatePbufferSurface(d, config, pa);
  if (c == EGL_NO_CONTEXT || s == EGL_NO_SURFACE ||
      !p_eglMakeCurrent(d, s, s, c)) {
    printf("CONTEXT FAIL 0x%x\n", p_eglGetError());
    return 2;
  }
  void *g =
      dlopen(getenv("PROBE_GLES") ?: "libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!g) {
    printf("GLES dlopen: %s\n", dlerror());
    return 2;
  }
  G(glGetString);
  G(glClearColor);
  G(glClear);
  G(glReadPixels);
  G(glGetError);
  printf("GL vendor=%s renderer=%s version=%s GLSL=%s\nextensions=%s\n",
         p_glGetString(GL_VENDOR), p_glGetString(GL_RENDERER),
         p_glGetString(GL_VERSION), p_glGetString(GL_SHADING_LANGUAGE_VERSION),
         p_glGetString(GL_EXTENSIONS));
  unsigned char pixel[4] = {0};
  p_glClearColor(1, 0, 0, 1);
  p_glClear(GL_COLOR_BUFFER_BIT);
  p_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  GLenum err = p_glGetError();
  int ok = pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 &&
           pixel[3] == 255 && err == 0;
  printf("CLEAR_READBACK %s rgba=%u,%u,%u,%u error=%x\n", ok ? "PASS" : "FAIL",
         pixel[0], pixel[1], pixel[2], pixel[3], err);
  G(glCreateShader);
  G(glShaderSource);
  G(glCompileShader);
  G(glGetShaderiv);
  G(glGetShaderInfoLog);
  G(glCreateProgram);
  G(glAttachShader);
  G(glBindAttribLocation);
  G(glLinkProgram);
  G(glGetProgramiv);
  G(glGetProgramInfoLog);
  G(glUseProgram);
  G(glGenBuffers);
  G(glBindBuffer);
  G(glBufferData);
  G(glVertexAttribPointer);
  G(glEnableVertexAttribArray);
  G(glViewport);
  G(glDrawArrays);
  G(glDeleteBuffers);
  G(glDeleteProgram);
  G(glDeleteShader);
  const char *sources[] = {
      "attribute vec2 pos; void main(){gl_Position=vec4(pos,0.0,1.0);}",
      "precision mediump float; void "
      "main(){gl_FragColor=vec4(0.0,1.0,0.0,1.0);}"};
  GLuint shaders[2];
  for (int i = 0; i < 2; i++) {
    shaders[i] = p_glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    p_glShaderSource(shaders[i], 1, &sources[i], NULL);
    p_glCompileShader(shaders[i]);
    GLint compiled = 0;
    p_glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
      char log[2048];
      p_glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
      printf("SHADER FAIL %s\n", log);
      return 2;
    }
  }
  GLuint program = p_glCreateProgram();
  p_glAttachShader(program, shaders[0]);
  p_glAttachShader(program, shaders[1]);
  p_glBindAttribLocation(program, 0, "pos");
  p_glLinkProgram(program);
  GLint linked = 0;
  p_glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[2048];
    p_glGetProgramInfoLog(program, sizeof(log), NULL, log);
    printf("LINK FAIL %s\n", log);
    return 2;
  }
  p_glUseProgram(program);
  float vertices[] = {-1, -1, 3, -1, -1, 3};
  GLuint vbo;
  p_glGenBuffers(1, &vbo);
  p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
  p_glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  p_glEnableVertexAttribArray(0);
  p_glViewport(0, 0, 16, 16);
  p_glDrawArrays(GL_TRIANGLES, 0, 3);
  p_glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  err = p_glGetError();
  int shader_ok = pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 &&
                  pixel[3] == 255 && err == 0;
  printf("SHADER_DRAW_READBACK %s rgba=%u,%u,%u,%u error=%x\n",
         shader_ok ? "PASS" : "FAIL", pixel[0], pixel[1], pixel[2], pixel[3],
         err);
  ok = ok && shader_ok;
  p_glDeleteBuffers(1, &vbo);
  p_glDeleteProgram(program);
  p_glDeleteShader(shaders[0]);
  p_glDeleteShader(shaders[1]);
  p_eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  p_eglDestroySurface(d, s);
  p_eglDestroyContext(d, c);
  p_eglTerminate(d);
  return ok ? 0 : 2;
}
