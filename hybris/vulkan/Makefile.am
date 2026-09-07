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
libhybris_vulkan_icd_la_SOURCES = icd/icd.c icd/instance.c icd/instance.h icd/device.c icd/device.h icd/hwvulkan.h icd/physical_commands.inc \
    compat/spirv_decorations.c compat/spirv_decorations.h compat/spirv_constants.c compat/spirv_constants.h compat/spirv_aggregate.c compat/spirv_aggregate.h compat/spirv_entry.c compat/spirv_entry.h compat/spirv_results.inc compat/spirv_literals.inc compat/scaled_dump.c compat/scaled_dispatch.c compat/scaled_dispatch.h compat/scaled_formats.c compat/scaled_formats.h compat/scaled_spirv.c compat/scaled_vertex.h
EXTRA_DIST = icd/README.md
libhybris_vulkan_icd_la_CFLAGS = $(libvulkan_la_CFLAGS) -fvisibility=hidden
libhybris_vulkan_icd_la_LDFLAGS = \
    -ldl -pthread -Wl,-z,nodelete \
    $(top_builddir)/common/libhybris-common.la \
    $(top_builddir)/hardware/libhardware.la \
    -version-info 0:0:0

endif
