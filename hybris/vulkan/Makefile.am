if HAS_VULKAN_HEADERS

bclicensedir = $(libdir)/licenses/libhybris-vulkan
bclicense_DATA = $(srcdir)/../../LICENSE.GPL3 compat/shaders/MALIBRIDGE.md compat/shaders/NOTICE.MaliBridge compat/shaders/LICENSE.bcdec compat/shaders/LICENSE.PanVK

lib_LTLIBRARIES = \
	libvulkan.la \
	libhybris-vulkan-icd.la \
	libVkLayer_hybris_compat.la

# Explicit loader layer, independent of Android linker/HAL/WSI libraries.
# Its manifest is colocated with the DSO; opt in using VK_LAYER_PATH.
libVkLayer_hybris_compat_la_SOURCES = layer/entry.c layer/device.c layer/physical.c layer/commands.c layer/layer.h \
    layer/device_features.c layer/device_features.h layer/device_create_sizes.inc \
    compat/bc_decode.c compat/bc_decode.h compat/shaders/bc_decode.inc \
    compat/bc_image.c compat/bc_image.h compat/bc_command_state.c compat/bc_command_state.h compat/bc_transfer.c compat/bc_transfer.h \
    compat/bc_context.c compat/bc_context.h compat/bc_commands.c compat/bc_resources.c compat/bc_record.c compat/bc_barriers.c compat/bc_image_copy.c compat/bc_policy.c compat/bc_policy.h \
    compat/application_policy.c compat/application_policy.h \
    compat/memory_visibility.c compat/memory_visibility.h compat/rendering_segments.c compat/rendering_segments.h \
    compat/readback_record.c compat/readback_submit.c compat/readback.h \
    compat/vertex_stores.c compat/vertex_stores.h compat/spirv_discard.c \
    compat/point_size.c compat/point_size.h \
    compat/spirv_inout.c compat/spirv_inout.h \
    compat/clip_distance.c compat/clip_distance.h compat/spirv_clip.c \
    compat/primitive_adapter.c compat/primitive_adapter.h \
    compat/vertex_compute.c compat/vertex_compute.h \
    compat/vertex_resources.c compat/vertex_resources.h \
    compat/polygon_pipeline.c compat/polygon_pipeline.h \
    compat/polygon_indices.c compat/polygon_indices.h compat/shaders/polygon_indices.inc \
    compat/polygon_layouts.c compat/polygon_layouts.h \
    compat/polygon_bindings.c compat/polygon_bindings.h \
    compat/polygon_raster.c compat/polygon_raster.h \
    compat/polygon_buffers.c compat/polygon_buffers.h \
    compat/shader_policy.c compat/shader_policy.h \
    compat/shader_cleanup.c compat/shader_cleanup.h compat/spirv_builtins.c compat/spirv_builtins.h \
    compat/spirv_decorations.c compat/spirv_decorations.h compat/spirv_constants.c compat/spirv_constants.h compat/spirv_aggregate.c compat/spirv_aggregate.h compat/spirv_entry.c compat/spirv_entry.h compat/spirv_results.inc compat/spirv_literals.inc compat/scaled_dump.c compat/shader_dispatch.c compat/shader_dispatch.h compat/scaled_formats.c compat/scaled_formats.h compat/scaled_spirv.c compat/scaled_vertex.h
libVkLayer_hybris_compat_la_CFLAGS = -fvisibility=hidden
# Keep process-wide policy initialization and lifetime generations across the
# loader unloading its last instance. All per-instance/device state is freed.
libVkLayer_hybris_compat_la_LDFLAGS = -module -avoid-version -no-undefined -pthread -Wl,-z,nodelete
layerdir = $(libdir)
layer_DATA = layer/VkLayer_hybris_compat.json

libvulkan_la_SOURCES = \
	vulkan.c \
	render_dispatch.c \
	timeline_dispatch.c \
	render_dispatch.h \
	vulkan_exports.c \
	vulkan_exports.h

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
libhybris_vulkan_icd_la_SOURCES = icd/icd.c icd/instance.c icd/instance.h icd/device.c icd/device.h icd/wsi.c icd/wsi.h icd/swapchain.c icd/swapchain.h icd/vk_android_native_buffer.h icd/hwvulkan.h icd/physical_commands.inc
EXTRA_DIST = compat/shaders/astc_repack.glsl compat/shaders/LICENSE.PanVK compat/shaders/bc_tables.glsl compat/shaders/astc_tables.glsl compat/shaders/astc_enc.glsl compat/shaders/etc2_enc.glsl compat/shaders/eac_enc.glsl compat/shaders/astc_hdr_enc.glsl compat/shaders/astcn_enc.glsl compat/shaders/MALIBRIDGE.md compat/shaders/LICENSE.bcdec icd/README.md compat/shaders/bc_decode.comp compat/shaders/generate-bc.py compat/shaders/polygon_indices.comp compat/shaders/generate-polygon.py layer/VkLayer_hybris_compat.json
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
    icd/native_window.h icd/native_window_wayland.c icd/native_window_headless.cpp \
    icd/window_owner.cpp icd/window_owner.h \
    icd/wayland_window.cpp icd/wayland_window.h \
    $(top_srcdir)/platforms/wayland/wayland_window_common.cpp
libhybris_vulkan_icd_la_CFLAGS += \
    -I$(top_srcdir)/platforms/common \
    -I$(top_srcdir)/platforms/wayland \
    -I$(top_srcdir)/vulkan \
    -I$(top_srcdir)/vulkan/icd \
    -I$(top_builddir)/platforms/common \
    $(ANDROID_HEADERS_CFLAGS) \
    $(WAYLAND_CLIENT_CFLAGS) $(WAYLAND_EGL_CFLAGS)
libhybris_vulkan_icd_la_CXXFLAGS += \
    -I$(top_srcdir)/platforms/common \
    -I$(top_srcdir)/platforms/wayland \
    -I$(top_srcdir)/vulkan \
    -I$(top_srcdir)/vulkan/icd \
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
