#define _GNU_SOURCE
#ifndef HYBRIS_PROBE_LINKED
#define VK_NO_PROTOTYPES
#endif
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "shaders/widget.vert.inc"
#include "shaders/widget.frag.inc"

__attribute__((constructor))
static void probe_watchdog(void) {
  alarm(25);
}

static void *sym(void *h, const char *n) {
  void *p = dlsym(h, n);
  if (!p) {
    printf("MISSING %s: %s\n", n, dlerror());
    exit(2);
  }
  return p;
}
#define E(n) __typeof__(&n) p_##n = sym(e, #n)
#define G(n) __typeof__(&n) p_##n = sym(g, #n)
#define V(n)                                                                   \
  PFN_##n p_##n = (PFN_##n)gip(instance, #n);                                  \
  if (!p_##n) {                                                                \
    printf("MISSING %s\n", #n);                                                \
    return 2;                                                                  \
  }
#define CHECK(x)                                                               \
  do {                                                                         \
    VkResult r = (x);                                                          \
    printf("%s = %d\n", #x, r);                                                \
    if (r != VK_SUCCESS)                                                       \
      return 2;                                                                \
  } while (0)

static int find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
                    VkMemoryPropertyFlags need) {
  for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
    if ((bits & (1u << i)) &&
        (mp->memoryTypes[i].propertyFlags & need) == need)
      return (int)i;
  return -1;
}

static int pick_queue(PFN_vkGetPhysicalDeviceQueueFamilyProperties qf,
                      VkPhysicalDevice pd, uint32_t *qi) {
  uint32_t count = 0;
  qf(pd, &count, NULL);
  VkQueueFamilyProperties *q = calloc(count, sizeof(*q));
  qf(pd, &count, q);
  uint32_t i = 0;
  while (i < count &&
         !(q[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
    i++;
  free(q);
  if (i == count)
    return 0;
  *qi = i;
  return 1;
}

static int eglprobe(int version) {
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
static int vkprobe(void) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkEnumerateInstanceExtensionProperties);
  uint32_t count = 0;
  CHECK(p_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
  VkExtensionProperties *ext = calloc(count, sizeof(*ext));
  CHECK(p_vkEnumerateInstanceExtensionProperties(NULL, &count, ext));
  for (uint32_t i = 0; i < count; i++)
    printf("INSTANCE_EXT %s\n", ext[i].extensionName);
  free(ext);
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-baseline",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceProperties);
  V(vkGetPhysicalDeviceFeatures);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkEnumerateDeviceExtensionProperties);
  V(vkDestroyInstance);
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  printf("DEVICE_COUNT %u\n", count);
  if (!count)
    return 2;
  VkPhysicalDevice devices[8];
  if (count > 8)
    count = 8;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { printf("No Vulkan physical devices\n"); return 2; }
  VkPhysicalDevice pd = devices[0];
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceFeatures f;
  p_vkGetPhysicalDeviceProperties(pd, &props);
  p_vkGetPhysicalDeviceFeatures(pd, &f);
  printf("GPU %s Vulkan=%u.%u.%u driver=0x%x BC=%u ETC2=%u ASTC=%u geometry=%u "
         "tessellation=%u float64=%u int64=%u\n",
         props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
         props.driverVersion, f.textureCompressionBC, f.textureCompressionETC2,
         f.textureCompressionASTC_LDR, f.geometryShader, f.tessellationShader,
         f.shaderFloat64, f.shaderInt64);
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL));
  ext = calloc(count, sizeof(*ext));
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &count, ext));
  for (uint32_t i = 0; i < count; i++)
    printf("DEVICE_EXT %s\n", ext[i].extensionName);
  free(ext);
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
  VkQueueFamilyProperties *q = calloc(count, sizeof(*q));
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, q);
  uint32_t qi = 0;
  while (qi < count &&
         !(q[qi].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
    qi++;
  free(q);
  if (qi == count)
    return 2;
  V(vkCreateDevice);
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  V(vkGetDeviceQueue);
  V(vkCreateBuffer);
  V(vkGetBufferMemoryRequirements);
  V(vkGetPhysicalDeviceMemoryProperties);
  V(vkAllocateMemory);
  V(vkBindBufferMemory);
  V(vkMapMemory);
  V(vkUnmapMemory);
  V(vkCreateCommandPool);
  V(vkAllocateCommandBuffers);
  V(vkBeginCommandBuffer);
  V(vkCmdFillBuffer);
  V(vkCmdPipelineBarrier);
  V(vkEndCommandBuffer);
  V(vkQueueSubmit);
  V(vkCreateFence);
  V(vkWaitForFences);
  V(vkDestroyFence);
  V(vkDestroyCommandPool);
  V(vkDestroyBuffer);
  V(vkFreeMemory);
  V(vkDestroyDevice);
  VkQueue queue;
  p_vkGetDeviceQueue(device, qi, 0, &queue);
  VkBuffer buffer;
  VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = 4096,
                           .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  CHECK(p_vkCreateBuffer(device, &bc, NULL, &buffer));
  VkMemoryRequirements mr;
  p_vkGetBufferMemoryRequirements(device, buffer, &mr);
  VkPhysicalDeviceMemoryProperties mp;
  p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  uint32_t mi = 0;
  for (; mi < mp.memoryTypeCount; mi++)
    if ((mr.memoryTypeBits & (1u << mi)) &&
        (mp.memoryTypes[mi].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      break;
  if (mi == mp.memoryTypeCount)
    return 3;
  VkDeviceMemory memory;
  VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = mr.size,
                             .memoryTypeIndex = mi};
  CHECK(p_vkAllocateMemory(device, &ma, NULL, &memory));
  CHECK(p_vkBindBufferMemory(device, buffer, memory, 0));
  VkCommandPool pool;
  VkCommandPoolCreateInfo pc = {.sType =
                                    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                .queueFamilyIndex = qi};
  CHECK(p_vkCreateCommandPool(device, &pc, NULL, &pool));
  VkCommandBuffer cb;
  VkCommandBufferAllocateInfo ca = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  CHECK(p_vkAllocateCommandBuffers(device, &ca, &cb));
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(p_vkBeginCommandBuffer(cb, &begin));
  p_vkCmdFillBuffer(cb, buffer, 0, 4096, 0x1234abcd);
  VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0,
                         NULL);
  CHECK(p_vkEndCommandBuffer(cb));
  VkFence fence;
  VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(p_vkCreateFence(device, &fc, NULL, &fence));
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                     .commandBufferCount = 1,
                     .pCommandBuffers = &cb};
  CHECK(p_vkQueueSubmit(queue, 1, &si, fence));
  CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
  void *mapped;
  CHECK(p_vkMapMemory(device, memory, 0, 4096, 0, &mapped));
  int ok = 1;
  for (int i = 0; i < 1024; i++)
    if (((uint32_t *)mapped)[i] != 0x1234abcd)
      ok = 0;
  printf("GPU_FILL_READBACK %s\n", ok ? "PASS" : "FAIL");
  p_vkUnmapMemory(device, memory);
  p_vkDestroyFence(device, fence, NULL);
  p_vkDestroyCommandPool(device, pool, NULL);
  p_vkDestroyBuffer(device, buffer, NULL);
  p_vkFreeMemory(device, memory, NULL);
  p_vkDestroyDevice(device, NULL);
  p_vkDestroyInstance(instance, NULL);
  return ok ? 0 : 2;
}

static int same_or_both(void *a, void *b, const char *label) {
  printf("ENTRY %s dlsym=%p gipa=%p\n", label, a, b);
  if (!a || !b) {
    printf("ENTRY %s missing\n", label);
    return 0;
  }
  return 1;
}

static int dispatch_probe(void) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = dlsym(h, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance create_dl = dlsym(h, "vkCreateInstance");
  PFN_vkEnumerateInstanceExtensionProperties enum_dl =
      dlsym(h, "vkEnumerateInstanceExtensionProperties");
  if (!gip) {
    printf("MISSING vkGetInstanceProcAddr via dlsym\n");
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip_gipa =
      (PFN_vkGetInstanceProcAddr)gip(NULL, "vkGetInstanceProcAddr");
  /* NULL-instance self lookup is optional before Vulkan 1.2. */
  (void)gip_gipa;
  PFN_vkCreateInstance create_gip =
      (PFN_vkCreateInstance)gip(NULL, "vkCreateInstance");
  PFN_vkEnumerateInstanceExtensionProperties enum_gip =
      (PFN_vkEnumerateInstanceExtensionProperties)gip(
          NULL, "vkEnumerateInstanceExtensionProperties");
  if (!same_or_both((void *)create_dl, (void *)create_gip, "vkCreateInstance") ||
      !same_or_both((void *)enum_dl, (void *)enum_gip,
                    "vkEnumerateInstanceExtensionProperties"))
    return 2;
#ifdef HYBRIS_PROBE_LINKED
  printf("LINK vkGetInstanceProcAddr=%p vkCreateInstance=%p\n",
         (void *)vkGetInstanceProcAddr, (void *)vkCreateInstance);
  if (vkGetInstanceProcAddr(NULL, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("LINK GIPA missing-symbol leaked\n");
    return 2;
  }
#endif
  if (gip(NULL, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("GIPA missing-symbol leaked\n");
    return 2;
  }
  const char *nonglobal[] = {"vkCreateDevice", "vkQueueSubmit", "vkDestroyInstance",
                            "vkCreateSwapchainKHR", "vkCreateWaylandSurfaceKHR"};
  for (unsigned i = 0; i < sizeof(nonglobal)/sizeof(nonglobal[0]); ++i) {
    if (gip(NULL, nonglobal[i])) {
      printf("GIPA_SCOPE FAIL %s without instance\n", nonglobal[i]);
      return 2;
    }
  }
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-dispatch",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  VkInstance instance = VK_NULL_HANDLE;
  CHECK(create_gip(&ci, NULL, &instance));
#ifdef HYBRIS_PROBE_LINKED
  {
    VkInstance second = VK_NULL_HANDLE;
    CHECK(vkCreateInstance(&ci, NULL, &second));
    vkDestroyInstance(second, NULL);
  }
#endif
  PFN_vkEnumeratePhysicalDevices enum_pd =
      (PFN_vkEnumeratePhysicalDevices)gip(instance, "vkEnumeratePhysicalDevices");
  PFN_vkGetDeviceProcAddr gdp =
      (PFN_vkGetDeviceProcAddr)gip(instance, "vkGetDeviceProcAddr");
  PFN_vkCreateDevice create_dev =
      (PFN_vkCreateDevice)gip(instance, "vkCreateDevice");
  PFN_vkGetPhysicalDeviceQueueFamilyProperties qf =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gip(
          instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  PFN_vkDestroyInstance destroy_inst =
      (PFN_vkDestroyInstance)gip(instance, "vkDestroyInstance");
  PFN_vkDestroyDevice destroy_dev =
      (PFN_vkDestroyDevice)gip(instance, "vkDestroyDevice");
  if (!enum_pd || !gdp || !create_dev || !qf || !destroy_inst || !destroy_dev) {
    printf("MISSING instance dispatch\n");
    return 2;
  }
  if (gip(instance, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("GIPA instance missing-symbol leaked\n");
    return 2;
  }
  uint32_t count = 0;
  CHECK(enum_pd(instance, &count, NULL));
  if (!count)
    return 2;
  VkPhysicalDevice devices[8];
  if (count > 8)
    count = 8;
  CHECK(enum_pd(instance, &count, devices));
  uint32_t qcount = 0;
  qf(devices[0], &qcount, NULL);
  VkQueueFamilyProperties *q = calloc(qcount, sizeof(*q));
  qf(devices[0], &qcount, q);
  uint32_t qi = 0;
  while (qi < qcount &&
         !(q[qi].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
    qi++;
  free(q);
  if (qi == qcount)
    return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  VkDevice device;
  CHECK(create_dev(devices[0], &dc, NULL, &device));
  PFN_vkGetDeviceProcAddr gdp_gdpa =
      (PFN_vkGetDeviceProcAddr)gdp(device, "vkGetDeviceProcAddr");
  if (!gdp_gdpa) {
    printf("GDPA vkGetDeviceProcAddr missing\n");
    return 2;
  }
  if (gdp(device, "vkHybrisDefinitelyMissing123") != NULL) {
    printf("GDPA missing-symbol leaked\n");
    return 2;
  }
  const char *nondevice[] = {"vkCreateInstance", "vkCreateDevice",
      "vkEnumeratePhysicalDevices", "vkGetPhysicalDeviceProperties",
      "vkDestroySurfaceKHR", "vkCreateWaylandSurfaceKHR", "vkCreateXcbSurfaceKHR",
      "vkCreateSwapchainKHR", "vkCmdBeginRenderingKHR", "vkQueueSubmit2KHR"};
  /* No device extensions were enabled. Their commands must not be exposed.
   * Later core commands may still be returned, but must not be called. */
  for (unsigned i = 0; i < sizeof(nondevice)/sizeof(nondevice[0]); ++i) {
    if (gdp(device, nondevice[i])) {
      printf("GDPA_SCOPE FAIL %s\n", nondevice[i]);
      return 2;
    }
  }
  PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)gdp(device, "vkQueueSubmit");
  void *begin_khr = (void *)gdp(device, "vkCmdBeginRenderingKHR");
  void *begin_core = (void *)gdp(device, "vkCmdBeginRendering");
  void *submit2 = (void *)gdp(device, "vkQueueSubmit2KHR");
  if (!submit) {
    printf("GDPA vkQueueSubmit missing\n");
    return 2;
  }
  printf("GDPA vkCmdBeginRenderingKHR %s\n", begin_khr ? "present" : "null");
  printf("GDPA vkCmdBeginRendering %s\n", begin_core ? "present" : "null");
  printf("GDPA vkQueueSubmit2KHR %s\n", submit2 ? "present" : "null");
  destroy_dev(device, NULL);
  destroy_inst(instance, NULL);
  printf("DISPATCH PASS\n");
  return 0;
}

struct life_ctx {
  PFN_vkGetInstanceProcAddr gip;
  VkInstance instance;
  VkPhysicalDevice pd;
  uint32_t qi;
  int rc;
};

static void *life_worker(void *arg) {
  struct life_ctx *c = arg;
  PFN_vkCreateDevice create_dev =
      (PFN_vkCreateDevice)c->gip(c->instance, "vkCreateDevice");
  PFN_vkDestroyDevice destroy_dev =
      (PFN_vkDestroyDevice)c->gip(c->instance, "vkDestroyDevice");
  PFN_vkGetDeviceQueue getq =
      (PFN_vkGetDeviceQueue)c->gip(c->instance, "vkGetDeviceQueue");
  PFN_vkCreateFence create_fence =
      (PFN_vkCreateFence)c->gip(c->instance, "vkCreateFence");
  PFN_vkDestroyFence destroy_fence =
      (PFN_vkDestroyFence)c->gip(c->instance, "vkDestroyFence");
  if (!create_dev || !destroy_dev || !getq || !create_fence || !destroy_fence) {
    c->rc = 2;
    return NULL;
  }
  for (int i = 0; i < 8; i++) {
    float priority = 1;
    VkDeviceQueueCreateInfo qc = {.sType =
                                      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = c->qi,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qc};
    VkDevice device;
    if (create_dev(c->pd, &dc, NULL, &device) != VK_SUCCESS) {
      c->rc = 2;
      return NULL;
    }
    VkQueue queue;
    getq(device, c->qi, 0, &queue);
    VkFence fence;
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (create_fence(device, &fc, NULL, &fence) != VK_SUCCESS) {
      destroy_dev(device, NULL);
      c->rc = 2;
      return NULL;
    }
    destroy_fence(device, fence, NULL);
    destroy_dev(device, NULL);
  }
  c->rc = 0;
  return NULL;
}

static int life_probe(int unload) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-life",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  if (!count)
    return 2;
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  uint32_t qi = 0;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, devices[0], &qi))
    return 2;
  /* A second dlopen references the same loaded library; it does not rerun constructors. */
  void *again =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!again) {
    printf("second dlopen failed: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip2 = dlsym(again, "vkGetInstanceProcAddr");
  if (!gip2)
    return 2;
  PFN_vkDestroyInstance destroy2 =
      (PFN_vkDestroyInstance)gip2(instance, "vkDestroyInstance");
  if (!destroy2) {
    printf("second dlopen lost instance dispatch\n");
    return 2;
  }
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  VkDevice a, b;
  CHECK(p_vkCreateDevice(devices[0], &dc, NULL, &a));
  CHECK(p_vkCreateDevice(devices[0], &dc, NULL, &b));
  if (a == b) {
    printf("two CreateDevice calls returned the same handle\n");
    return 2;
  }
  printf("DEVICES a=%p b=%p\n", (void *)a, (void *)b);
  p_vkDestroyDevice(a, NULL);
  p_vkDestroyDevice(b, NULL);
  /* Basic device recreation only; no per-object generation state is tested. */
  VkDevice c;
  CHECK(p_vkCreateDevice(devices[0], &dc, NULL, &c));
  p_vkDestroyDevice(c, NULL);
  struct life_ctx workers[2] = {
      {.gip = gip, .instance = instance, .pd = devices[0], .qi = qi, .rc = 1},
      {.gip = gip, .instance = instance, .pd = devices[0], .qi = qi, .rc = 1},
  };
  pthread_t t[2];
  int started = 0;
  for (; started < 2; ++started) {
    int err = pthread_create(&t[started], NULL, life_worker, &workers[started]);
    if (err) {
      for (int j = 0; j < started; ++j) pthread_join(t[j], NULL);
      printf("pthread_create failed: %s\n", strerror(err));
      return 2;
    }
  }
  pthread_join(t[0], NULL);
  pthread_join(t[1], NULL);
  if (workers[0].rc || workers[1].rc) {
    printf("LIFE thread rc=%d %d\n", workers[0].rc, workers[1].rc);
    return 2;
  }
  p_vkDestroyInstance(instance, NULL);
  /* Recreate instance while the library remains loaded. */
  VkInstance second = VK_NULL_HANDLE;
  CHECK(p_vkCreateInstance(&ci, NULL, &second));
  p_vkDestroyInstance(second, NULL);
  dlclose(again);
  if (unload) {
    printf("LIFE closing final library reference; process exit is part of the check\n");
    dlclose(h);
  }
  /* Ordinary life mode retains the library until process exit, like other probes. */
  printf("LIFE operations complete\n");
  return 0;
}

static int caps_probe(void) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-caps",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceFeatures);
  V(vkGetPhysicalDeviceProperties);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkGetPhysicalDeviceFormatProperties);
  V(vkEnumerateDeviceExtensionProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  V(vkGetDeviceProcAddr);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { printf("No Vulkan physical devices\n"); return 2; }
  VkPhysicalDevice pd = devices[0];
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceFeatures features;
  p_vkGetPhysicalDeviceProperties(pd, &props);
  p_vkGetPhysicalDeviceFeatures(pd, &features);
  printf("CAPS gpu=%s api=%u.%u.%u maxPush=%u minUboAlign=%u BC=%u\n",
         props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
         props.limits.maxPushConstantsSize,
         (unsigned)props.limits.minUniformBufferOffsetAlignment,
         features.textureCompressionBC);
  uint32_t ext_count = 0;
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, NULL));
  VkExtensionProperties *ext = calloc(ext_count, sizeof(*ext));
  CHECK(p_vkEnumerateDeviceExtensionProperties(pd, NULL, &ext_count, ext));
  int has_dyn = 0, has_sync2 = 0;
  for (uint32_t i = 0; i < ext_count; i++) {
    if (!strcmp(ext[i].extensionName, "VK_KHR_dynamic_rendering"))
      has_dyn = 1;
    if (!strcmp(ext[i].extensionName, "VK_KHR_synchronization2"))
      has_sync2 = 1;
  }
  printf("CAPS ext dynamic_rendering=%d synchronization2=%d count=%u\n",
         has_dyn, has_sync2, ext_count);
  free(ext);
  VkFormatProperties fmt;
  p_vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fmt);
  printf("CAPS R8G8B8A8_UNORM linear=0x%x optimal=0x%x buffer=0x%x\n",
         fmt.linearTilingFeatures, fmt.optimalTilingFeatures,
         fmt.bufferFeatures);
  uint32_t qi = 0;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, pd, &qi))
    return 2;
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  /* Refuse a feature the query said is false. Do not strip pNext and retry. */
  if (!features.shaderFloat64) {
    VkPhysicalDeviceFeatures want = features;
    want.shaderFloat64 = VK_TRUE;
    VkDeviceCreateInfo bad = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qc,
                              .pEnabledFeatures = &want};
    VkDevice rejected = VK_NULL_HANDLE;
    VkResult br = p_vkCreateDevice(pd, &bad, NULL, &rejected);
    printf("CAPS enable-unadvertised-float64 = %d\n", br);
    if (br != VK_ERROR_FEATURE_NOT_PRESENT) {
      printf("CAPS expected VK_ERROR_FEATURE_NOT_PRESENT\n");
      if (br == VK_SUCCESS) p_vkDestroyDevice(rejected, NULL);
      return 2;
    }
  }
  const char *ghost = "VK_KHR_hybris_not_a_real_extension";
  VkDeviceCreateInfo ghost_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 .queueCreateInfoCount = 1,
                                 .pQueueCreateInfos = &qc,
                                 .enabledExtensionCount = 1,
                                 .ppEnabledExtensionNames = &ghost};
  VkDevice ghost_dev = VK_NULL_HANDLE;
  VkResult gr = p_vkCreateDevice(pd, &ghost_ci, NULL, &ghost_dev);
  printf("CAPS enable-unknown-extension = %d\n", gr);
  if (gr != VK_ERROR_EXTENSION_NOT_PRESENT) {
    printf("CAPS expected VK_ERROR_EXTENSION_NOT_PRESENT\n");
    if (gr == VK_SUCCESS) p_vkDestroyDevice(ghost_dev, NULL);
    return 2;
  }
  VkDeviceCreateInfo ok_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qc,
                              .pEnabledFeatures = &features};
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &ok_ci, NULL, &device));
  PFN_vkGetDeviceProcAddr gdp =
      (PFN_vkGetDeviceProcAddr)gip(instance, "vkGetDeviceProcAddr");
  /* Availability is not enablement: this device enabled no extensions. */
  if (gdp(device, "vkCmdBeginRenderingKHR") != NULL) {
    printf("CAPS dynamic_rendering not enabled but GDPA present\n");
    p_vkDestroyDevice(device, NULL);
    return 2;
  }
  p_vkDestroyDevice(device, NULL);
  p_vkDestroyInstance(instance, NULL);
  printf("CAPS PASS (queries and rejection checks; no cross-backend comparison)\n");
  return 0;
}

enum {
  kWidgetUboBytes = 272,
  kWidgetIndexCount = 18,
  kWidgetImage = 16
};

struct widget_ubo {
  float parameters[12][4];
  float mvp[16];
  float checker[3];
  int srgbTarget;
};

static int ubo_draw(int inject_wrong_binding) {
  void *h =
      dlopen(getenv("PROBE_VK") ?: "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("Vulkan dlopen: %s\n", dlerror());
    return 2;
  }
  PFN_vkGetInstanceProcAddr gip = sym(h, "vkGetInstanceProcAddr");
  VkInstance instance = VK_NULL_HANDLE;
  V(vkCreateInstance);
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "hybris-ubo",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  CHECK(p_vkCreateInstance(&ci, NULL, &instance));
  V(vkDestroyInstance);
  V(vkEnumeratePhysicalDevices);
  V(vkGetPhysicalDeviceMemoryProperties);
  V(vkGetPhysicalDeviceQueueFamilyProperties);
  V(vkCreateDevice);
  V(vkDestroyDevice);
  V(vkGetDeviceQueue);
  V(vkCreateBuffer);
  V(vkDestroyBuffer);
  V(vkGetBufferMemoryRequirements);
  V(vkAllocateMemory);
  V(vkFreeMemory);
  V(vkBindBufferMemory);
  V(vkMapMemory);
  V(vkUnmapMemory);
  V(vkCreateImage);
  V(vkDestroyImage);
  V(vkGetImageMemoryRequirements);
  V(vkBindImageMemory);
  V(vkCreateImageView);
  V(vkDestroyImageView);
  V(vkCreateShaderModule);
  V(vkDestroyShaderModule);
  V(vkCreateDescriptorSetLayout);
  V(vkDestroyDescriptorSetLayout);
  V(vkCreatePipelineLayout);
  V(vkDestroyPipelineLayout);
  V(vkCreateRenderPass);
  V(vkDestroyRenderPass);
  V(vkCreateFramebuffer);
  V(vkDestroyFramebuffer);
  V(vkCreateGraphicsPipelines);
  V(vkDestroyPipeline);
  V(vkCreateDescriptorPool);
  V(vkDestroyDescriptorPool);
  V(vkAllocateDescriptorSets);
  V(vkUpdateDescriptorSets);
  V(vkCreateCommandPool);
  V(vkDestroyCommandPool);
  V(vkAllocateCommandBuffers);
  V(vkBeginCommandBuffer);
  V(vkEndCommandBuffer);
  V(vkCmdBeginRenderPass);
  V(vkCmdEndRenderPass);
  V(vkCmdBindPipeline);
  V(vkCmdBindDescriptorSets);
  V(vkCmdBindIndexBuffer);
  V(vkCmdDrawIndexed);
  V(vkCmdCopyImageToBuffer);
  V(vkCmdPipelineBarrier);
  V(vkCreateFence);
  V(vkDestroyFence);
  V(vkQueueSubmit);
  V(vkWaitForFences);
  uint32_t count = 0;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, NULL));
  VkPhysicalDevice devices[4];
  if (count > 4)
    count = 4;
  CHECK(p_vkEnumeratePhysicalDevices(instance, &count, devices));
  if (!count) { printf("No Vulkan physical devices\n"); return 2; }
  VkPhysicalDevice pd = devices[0];
  uint32_t qi = 0;
  if (!pick_queue(p_vkGetPhysicalDeviceQueueFamilyProperties, pd, &qi))
    return 2;
  uint32_t queue_count = 0;
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &queue_count, NULL);
  VkQueueFamilyProperties *queues = calloc(queue_count, sizeof(*queues));
  if (!queues) return 2;
  p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &queue_count, queues);
  for (qi = 0; qi < queue_count; ++qi)
    if (queues[qi].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
  free(queues);
  if (qi == queue_count) { printf("No graphics queue\n"); return 3; }
  float priority = 1;
  VkDeviceQueueCreateInfo qc = {.sType =
                                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                .queueFamilyIndex = qi,
                                .queueCount = 1,
                                .pQueuePriorities = &priority};
  VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                           .queueCreateInfoCount = 1,
                           .pQueueCreateInfos = &qc};
  VkDevice device;
  CHECK(p_vkCreateDevice(pd, &dc, NULL, &device));
  VkQueue queue;
  p_vkGetDeviceQueue(device, qi, 0, &queue);
  VkPhysicalDeviceMemoryProperties mp;
  p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);

  struct widget_ubo good = {0};
  struct widget_ubo bad = {0};
  good.parameters[0][0] = 1.0f;
  good.mvp[0] = 1.0f;
  good.mvp[5] = 1.0f;
  good.mvp[10] = 1.0f;
  good.mvp[15] = 1.0f;
  good.checker[0] = 0.0f;
  good.srgbTarget = 1;
  /* Keep identity MVP so the triangle still covers the readback pixel.
   * Only fragment-encoded fields differ. */
  bad.parameters[0][0] = 0.0f;
  bad.mvp[0] = 1.0f;
  bad.mvp[5] = 1.0f;
  bad.mvp[10] = 1.0f;
  bad.mvp[15] = 1.0f;
  bad.checker[0] = 1.0f;
  bad.srgbTarget = 0;
  if (sizeof(good) != kWidgetUboBytes) {
    printf("UBO sizeof=%zu expected=%d\n", sizeof(good), kWidgetUboBytes);
    return 2;
  }
  printf("UBO layout parameters@0 mvp@192 checker@256 srgb@268 size=%zu\n",
         sizeof(good));

  VkBufferCreateInfo ubo_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = kWidgetUboBytes,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
  VkBuffer ubo_good, ubo_bad;
  CHECK(p_vkCreateBuffer(device, &ubo_ci, NULL, &ubo_good));
  CHECK(p_vkCreateBuffer(device, &ubo_ci, NULL, &ubo_bad));
  VkMemoryRequirements ubo_mr;
  p_vkGetBufferMemoryRequirements(device, ubo_good, &ubo_mr);
  int umi = find_mem(&mp, ubo_mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (umi < 0)
    return 3;
  VkMemoryAllocateInfo uma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = ubo_mr.size,
                              .memoryTypeIndex = (uint32_t)umi};
  VkDeviceMemory umem_good, umem_bad;
  CHECK(p_vkAllocateMemory(device, &uma, NULL, &umem_good));
  CHECK(p_vkAllocateMemory(device, &uma, NULL, &umem_bad));
  CHECK(p_vkBindBufferMemory(device, ubo_good, umem_good, 0));
  CHECK(p_vkBindBufferMemory(device, ubo_bad, umem_bad, 0));
  void *mapped;
  CHECK(p_vkMapMemory(device, umem_good, 0, kWidgetUboBytes, 0, &mapped));
  memcpy(mapped, &good, sizeof(good));
  p_vkUnmapMemory(device, umem_good);
  CHECK(p_vkMapMemory(device, umem_bad, 0, kWidgetUboBytes, 0, &mapped));
  memcpy(mapped, &bad, sizeof(bad));
  p_vkUnmapMemory(device, umem_bad);

  uint16_t indices[kWidgetIndexCount] = {0, 1, 2, 0, 2, 3, 4, 5, 6,
                                         4, 6, 7, 8, 9, 10, 8, 10, 11};
  VkBufferCreateInfo ib_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = sizeof(indices),
                              .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
  VkBuffer ibo;
  CHECK(p_vkCreateBuffer(device, &ib_ci, NULL, &ibo));
  VkMemoryRequirements ib_mr;
  p_vkGetBufferMemoryRequirements(device, ibo, &ib_mr);
  int imi = find_mem(&mp, ib_mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (imi < 0) { printf("No coherent index memory\n"); return 3; }
  VkMemoryAllocateInfo ima = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = ib_mr.size,
                              .memoryTypeIndex = (uint32_t)imi};
  VkDeviceMemory imem;
  CHECK(p_vkAllocateMemory(device, &ima, NULL, &imem));
  CHECK(p_vkBindBufferMemory(device, ibo, imem, 0));
  CHECK(p_vkMapMemory(device, imem, 0, sizeof(indices), 0, &mapped));
  memcpy(mapped, indices, sizeof(indices));
  p_vkUnmapMemory(device, imem);

  VkImageCreateInfo img_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = {kWidgetImage, kWidgetImage, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  VkImage image;
  CHECK(p_vkCreateImage(device, &img_ci, NULL, &image));
  VkMemoryRequirements img_mr;
  p_vkGetImageMemoryRequirements(device, image, &img_mr);
  int img_mi = find_mem(&mp, img_mr.memoryTypeBits,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (img_mi < 0)
    img_mi = find_mem(&mp, img_mr.memoryTypeBits, 0);
  if (img_mi < 0) { printf("No compatible image memory\n"); return 3; }
  VkMemoryAllocateInfo img_ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = img_mr.size,
                                 .memoryTypeIndex = (uint32_t)img_mi};
  VkDeviceMemory img_mem;
  CHECK(p_vkAllocateMemory(device, &img_ma, NULL, &img_mem));
  CHECK(p_vkBindImageMemory(device, image, img_mem, 0));
  VkImageViewCreateInfo view_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  VkImageView view;
  CHECK(p_vkCreateImageView(device, &view_ci, NULL, &view));

  VkBufferCreateInfo rb_ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = kWidgetImage * kWidgetImage * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VkBuffer readback;
  CHECK(p_vkCreateBuffer(device, &rb_ci, NULL, &readback));
  VkMemoryRequirements rb_mr;
  p_vkGetBufferMemoryRequirements(device, readback, &rb_mr);
  int rmi = find_mem(&mp, rb_mr.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (rmi < 0) { printf("No coherent readback memory\n"); return 3; }
  VkMemoryAllocateInfo rma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = rb_mr.size,
                              .memoryTypeIndex = (uint32_t)rmi};
  VkDeviceMemory rmem;
  CHECK(p_vkAllocateMemory(device, &rma, NULL, &rmem));
  CHECK(p_vkBindBufferMemory(device, readback, rmem, 0));

  VkShaderModuleCreateInfo vs_ci = {.sType =
                                        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = kWidgetVertSpv_word_count * 4,
                                    .pCode = kWidgetVertSpv};
  VkShaderModuleCreateInfo fs_ci = {.sType =
                                        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = kWidgetFragSpv_word_count * 4,
                                    .pCode = kWidgetFragSpv};
  VkShaderModule vs, fs;
  CHECK(p_vkCreateShaderModule(device, &vs_ci, NULL, &vs));
  CHECK(p_vkCreateShaderModule(device, &fs_ci, NULL, &fs));
  VkDescriptorSetLayoutBinding bind = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
  VkDescriptorSetLayoutCreateInfo sl_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &bind};
  VkDescriptorSetLayout set_layout;
  CHECK(p_vkCreateDescriptorSetLayout(device, &sl_ci, NULL, &set_layout));
  VkPipelineLayoutCreateInfo pl_ci = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &set_layout};
  VkPipelineLayout pipeline_layout;
  CHECK(p_vkCreatePipelineLayout(device, &pl_ci, NULL, &pipeline_layout));
  VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                 .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                 .finalLayout =
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
  VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color_ref};
  VkSubpassDependency to_copy = {
      .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
  VkRenderPassCreateInfo rp_ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .dependencyCount = 1,
                                  .pDependencies = &to_copy,
                                  .attachmentCount = 1,
                                  .pAttachments = &att,
                                  .subpassCount = 1,
                                  .pSubpasses = &sub};
  VkRenderPass rp;
  CHECK(p_vkCreateRenderPass(device, &rp_ci, NULL, &rp));
  VkFramebufferCreateInfo fb_ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                   .renderPass = rp,
                                   .attachmentCount = 1,
                                   .pAttachments = &view,
                                   .width = kWidgetImage,
                                   .height = kWidgetImage,
                                   .layers = 1};
  VkFramebuffer fb;
  CHECK(p_vkCreateFramebuffer(device, &fb_ci, NULL, &fb));
  VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vs,
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = fs,
       .pName = "main"}};
  VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport vp = {0, 0, (float)kWidgetImage, (float)kWidgetImage, 0, 1};
  VkRect2D sc = {{0, 0}, {kWidgetImage, kWidgetImage}};
  VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &vp,
      .scissorCount = 1,
      .pScissors = &sc};
  VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1};
  VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
  VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &cba};
  VkGraphicsPipelineCreateInfo gp = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vi,
      .pInputAssemblyState = &ia,
      .pViewportState = &vps,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pColorBlendState = &blend,
      .layout = pipeline_layout,
      .renderPass = rp};
  VkPipeline pipeline;
  CHECK(p_vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL,
                                    &pipeline));
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size};
  VkDescriptorPool pool;
  CHECK(p_vkCreateDescriptorPool(device, &pool_ci, NULL, &pool));
  VkDescriptorSetAllocateInfo sa = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &set_layout};
  VkDescriptorSet set;
  CHECK(p_vkAllocateDescriptorSets(device, &sa, &set));
  VkDescriptorBufferInfo dbi = {.buffer = inject_wrong_binding ? ubo_bad
                                                               : ubo_good,
                                .offset = 0,
                                .range = kWidgetUboBytes};
  VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .dstBinding = 0,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                .pBufferInfo = &dbi};
  p_vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
  printf("DRAW set=%p layout=%p pipeline=%p buffer=%p range=%u inject=%d\n",
         (void *)set, (void *)pipeline_layout, (void *)pipeline,
         (void *)dbi.buffer, kWidgetUboBytes, inject_wrong_binding);

  VkCommandPoolCreateInfo cpc = {.sType =
                                     VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                 .queueFamilyIndex = qi};
  VkCommandPool cpool;
  CHECK(p_vkCreateCommandPool(device, &cpc, NULL, &cpool));
  VkCommandBufferAllocateInfo cba_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = cpool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  VkCommandBuffer cb;
  CHECK(p_vkAllocateCommandBuffers(device, &cba_info, &cb));
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(p_vkBeginCommandBuffer(cb, &begin));
  VkClearValue clear = {.color = {{0, 0, 0, 0}}};
  VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = rp,
                                .framebuffer = fb,
                                .renderArea = {{0, 0}, {kWidgetImage, kWidgetImage}},
                                .clearValueCount = 1,
                                .pClearValues = &clear};
  p_vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                            0, 1, &set, 0, NULL);
  p_vkCmdBindIndexBuffer(cb, ibo, 0, VK_INDEX_TYPE_UINT16);
  p_vkCmdDrawIndexed(cb, kWidgetIndexCount, 1, 0, 0, 0);
  p_vkCmdEndRenderPass(cb);
  VkBufferImageCopy copy = {
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .imageExtent = {kWidgetImage, kWidgetImage, 1}};
  p_vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback, 1, &copy);
  VkMemoryBarrier to_host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, NULL, 0, NULL);
  CHECK(p_vkEndCommandBuffer(cb));
  VkFence fence;
  VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(p_vkCreateFence(device, &fc, NULL, &fence));
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                     .commandBufferCount = 1,
                     .pCommandBuffers = &cb};
  CHECK(p_vkQueueSubmit(queue, 1, &si, fence));
  CHECK(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
  uint8_t *pixels = NULL;
  CHECK(p_vkMapMemory(device, rmem, 0, kWidgetImage * kWidgetImage * 4, 0,
                      (void **)&pixels));
  uint8_t *mid = pixels + (8 * kWidgetImage + 8) * 4;
  printf("PIXEL mid rgba=%u,%u,%u,%u inject=%d\n", mid[0], mid[1], mid[2],
         mid[3], inject_wrong_binding);
  int match_good = mid[0] == 255 && mid[1] == 255 && mid[2] == 0 && mid[3] == 255;
  int match_bad = mid[0] == 0 && mid[1] == 255 && mid[2] == 255 && mid[3] == 0;
  p_vkUnmapMemory(device, rmem);
  p_vkDestroyFence(device, fence, NULL);
  p_vkDestroyCommandPool(device, cpool, NULL);
  p_vkDestroyPipeline(device, pipeline, NULL);
  p_vkDestroyFramebuffer(device, fb, NULL);
  p_vkDestroyRenderPass(device, rp, NULL);
  p_vkDestroyPipelineLayout(device, pipeline_layout, NULL);
  p_vkDestroyDescriptorPool(device, pool, NULL);
  p_vkDestroyDescriptorSetLayout(device, set_layout, NULL);
  p_vkDestroyShaderModule(device, vs, NULL);
  p_vkDestroyShaderModule(device, fs, NULL);
  p_vkDestroyImageView(device, view, NULL);
  p_vkDestroyImage(device, image, NULL);
  p_vkDestroyBuffer(device, readback, NULL);
  p_vkDestroyBuffer(device, ibo, NULL);
  p_vkDestroyBuffer(device, ubo_good, NULL);
  p_vkDestroyBuffer(device, ubo_bad, NULL);
  p_vkFreeMemory(device, img_mem, NULL);
  p_vkFreeMemory(device, rmem, NULL);
  p_vkFreeMemory(device, imem, NULL);
  p_vkFreeMemory(device, umem_good, NULL);
  p_vkFreeMemory(device, umem_bad, NULL);
  p_vkDestroyDevice(device, NULL);
  p_vkDestroyInstance(instance, NULL);
  if (inject_wrong_binding) {
    if (!match_bad) {
      printf("UBO negative-control FAIL (unexpected pixel)\n");
      return 2;
    }
    printf("UBO negative-control PASS (known alternate descriptor observed)\n");
    return 0;
  }
  if (!match_good) {
    printf("UBO shader did not read parameters/mvp/srgb\n");
    return 2;
  }
  printf("UBO shader-read PASS\n");
  return 0;
}

static int ubo_probe(void) {
  int good = ubo_draw(0);
  if (good)
    return good;
  int bad = ubo_draw(1);
  if (bad)
    return bad;
  printf("UBO PASS\n");
  return 0;
}

int main(int argc, char **argv) {
  setbuf(stdout, NULL);
  const char *mode = argc > 1 ? argv[1] : "vk";
  printf("PROBE pid=%d mode=%s linked=%d\n", getpid(), mode,
#ifdef HYBRIS_PROBE_LINKED
         1
#else
         0
#endif
  );
  int rc;
  if (!strcmp(mode, "dispatch"))
    rc = dispatch_probe();
  else if (!strcmp(mode, "life"))
    rc = life_probe(0);
  else if (!strcmp(mode, "unload"))
    rc = life_probe(1);
  else if (!strcmp(mode, "caps"))
    rc = caps_probe();
  else if (!strcmp(mode, "ubo"))
    rc = ubo_probe();
  else if (!strcmp(mode, "vk"))
    rc = vkprobe();
  else if (!strcmp(mode, "0") || !strcmp(mode, "2") || !strcmp(mode, "3"))
    rc = eglprobe(atoi(mode));
  else {
    fprintf(stderr, "Unknown probe mode: %s\n", mode);
    rc = 2;
  }
  printf("RESULT %d\n", rc);
  return rc;
}
