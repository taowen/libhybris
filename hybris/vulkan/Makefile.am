if HAS_VULKAN_HEADERS

SUBDIRS = platforms

lib_LTLIBRARIES = \
	libvulkan.la \
	libhybris-vulkan-icd.la

libvulkan_la_SOURCES = \
	vulkan.c \
	render_dispatch.c \
	timeline_dispatch.c \
	render_dispatch.h \
	vulkan_exports.c \
	vulkan_exports.h \
	ws.c

libvulkan_la_CFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/common $(ANDROID_HEADERS_CFLAGS) -DPKGLIBDIR="\"$(pkglibdir)/\""
if WANT_TRACE
libvulkan_la_CFLAGS += -DDEBUG
endif
if WANT_DEBUG
libvulkan_la_CFLAGS += -ggdb -O0
endif

libvulkan_la_CXXFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/common $(ANDROID_HEADERS_CFLAGS) -DPKGLIBDIR="\"$(pkglibdir)/\""
if WANT_TRACE
libvulkan_la_CXXFLAGS += -DDEBUG
endif
if WANT_DEBUG
libvulkan_la_CXXFLAGS += -ggdb -O0
endif
libvulkan_la_LDFLAGS = \
	-ldl -pthread \
	$(top_builddir)/common/libhybris-common.la \
	$(top_builddir)/hardware/libhardware.la \
	-version-info 3:183:2

# Optional standard-loader backend; no default ICD JSON is installed.
libhybris_vulkan_icd_la_SOURCES = icd/icd.c icd/instance.c icd/instance.h icd/device.c icd/device.h icd/wsi.c icd/wsi.h icd/swapchain.c icd/swapchain.h icd/vk_android_native_buffer.h icd/hwvulkan.h icd/physical_commands.inc \
    compat/bc_decode.c compat/bc_decode.h compat/shaders/bc_decode.inc \
    compat/bc_image.c compat/bc_image.h compat/bc_command_state.c compat/bc_command_state.h compat/bc_transfer.c compat/bc_transfer.h \
    compat/bc_context.c compat/bc_context.h compat/bc_commands.c compat/bc_resources.c compat/bc_record.c compat/bc_barriers.c compat/bc_image_copy.c compat/bc_policy.c compat/bc_policy.h \
    compat/spirv_decorations.c compat/spirv_decorations.h compat/spirv_constants.c compat/spirv_constants.h compat/spirv_aggregate.c compat/spirv_aggregate.h compat/spirv_entry.c compat/spirv_entry.h compat/spirv_results.inc compat/spirv_literals.inc compat/scaled_dump.c compat/scaled_dispatch.c compat/scaled_dispatch.h compat/scaled_formats.c compat/scaled_formats.h compat/scaled_spirv.c compat/scaled_vertex.h
EXTRA_DIST = icd/README.md compat/shaders/bc_decode.comp compat/shaders/generate-bc.py
libhybris_vulkan_icd_la_CFLAGS = $(libvulkan_la_CFLAGS) -fvisibility=hidden
libhybris_vulkan_icd_la_CXXFLAGS = $(libvulkan_la_CXXFLAGS) -fvisibility=hidden
libhybris_vulkan_icd_la_LDFLAGS = \
    -ldl -pthread -Wl,-z,nodelete \
    $(top_builddir)/common/libhybris-common.la \
    $(top_builddir)/hardware/libhardware.la \
    -version-info 0:0:0
if WANT_WAYLAND
if WANT_X11
libhybris_vulkan_icd_la_SOURCES += icd/native_window_x11.cpp
libhybris_vulkan_icd_la_CFLAGS += $(XCB_CFLAGS) $(X11_CFLAGS)
libhybris_vulkan_icd_la_CXXFLAGS += $(XCB_CFLAGS) $(X11_CFLAGS)
libhybris_vulkan_icd_la_LDFLAGS += -lxcb -lX11-xcb -lX11
endif
libhybris_vulkan_icd_la_SOURCES += \
    icd/native_window.h icd/native_window_wayland.c \
    platforms/wayland/window_owner.cpp platforms/wayland/window_owner.h \
    platforms/wayland/wayland_window.cpp \
    $(top_srcdir)/platforms/wayland/wayland_window_common.cpp
libhybris_vulkan_icd_la_CFLAGS += \
    -I$(top_srcdir)/platforms/common \
    -I$(top_srcdir)/platforms/wayland \
    -I$(top_srcdir)/vulkan \
    -I$(top_srcdir)/vulkan/platforms/wayland \
    -I$(top_builddir)/platforms/common \
    $(ANDROID_HEADERS_CFLAGS) \
    $(WAYLAND_CLIENT_CFLAGS) $(WAYLAND_EGL_CFLAGS)
libhybris_vulkan_icd_la_CXXFLAGS += \
    -I$(top_srcdir)/platforms/common \
    -I$(top_srcdir)/platforms/wayland \
    -I$(top_srcdir)/vulkan \
    -I$(top_srcdir)/vulkan/platforms/common \
    -I$(top_srcdir)/vulkan/platforms/wayland \
    -I$(top_builddir)/platforms/common \
    $(ANDROID_HEADERS_CFLAGS) \
    $(WAYLAND_CLIENT_CFLAGS) $(WAYLAND_EGL_CFLAGS)
if WANT_TRACE
libhybris_vulkan_icd_la_CXXFLAGS += -DDEBUG
endif
if !WANT_WL_SERVERSIDE_BUFFERS
libhybris_vulkan_icd_la_CXXFLAGS += -DHYBRIS_NO_SERVER_SIDE_BUFFERS
endif
libhybris_vulkan_icd_la_LDFLAGS += \
    $(top_builddir)/platforms/common/libhybris-platformcommon.la \
    $(top_builddir)/gralloc/libgralloc.la \
    $(WAYLAND_CLIENT_LIBS) \
    $(WAYLAND_EGL_LIBS)
if HAS_ANDROID_4_2_0
libhybris_vulkan_icd_la_LDFLAGS += $(top_builddir)/libsync/libsync.la
endif
if HAS_ANDROID_5_0_0
libhybris_vulkan_icd_la_LDFLAGS += $(top_builddir)/libsync/libsync.la
endif
endif

endif
