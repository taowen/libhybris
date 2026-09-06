#define _GNU_SOURCE
#ifndef HYBRIS_PROBE_LINKED
#define VK_NO_PROTOTYPES
#endif
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

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
  else if (!strcmp(mode, "vk"))
    rc = vkprobe();
  else
    rc = eglprobe(atoi(mode));
  printf("RESULT %d\n", rc);
  return rc;
}
