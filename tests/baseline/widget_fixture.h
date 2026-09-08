#ifndef HYBRIS_WIDGET_FIXTURE_H
#define HYBRIS_WIDGET_FIXTURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

struct large_widget_ubo {
  struct widget_ubo widget;
  float matrices[14][16];
  float tail[3][4];
  int32_t signed_tag;
  uint32_t enabled;
  float end_marker[2];
};
_Static_assert(sizeof(struct large_widget_ubo) == 1232, "large std140 size");
_Static_assert(offsetof(struct large_widget_ubo, matrices) == 272, "matrix array offset");
_Static_assert(sizeof(((struct large_widget_ubo *)0)->matrices[0]) == 64, "matrix stride");
_Static_assert(offsetof(struct large_widget_ubo, tail) == 1168, "tail offset");
_Static_assert(offsetof(struct large_widget_ubo, signed_tag) == 1216, "int offset");
_Static_assert(offsetof(struct large_widget_ubo, enabled) == 1220, "bool storage offset");
_Static_assert(offsetof(struct large_widget_ubo, end_marker) == 1224, "end offset");

_Static_assert(sizeof(struct widget_ubo) == kWidgetUboBytes, "widget std140 size");
_Static_assert(offsetof(struct widget_ubo, mvp) == 192, "MVP offset");
_Static_assert(offsetof(struct widget_ubo, checker) == 256, "checker offset");
_Static_assert(offsetof(struct widget_ubo, srgbTarget) == 268, "int offset");

/* Both outputs own their storage. The small variant uses only the prefix. */
static uint32_t widget_fixture_data(int large, int dynamic,
                                    struct large_widget_ubo *good_out,
                                    struct large_widget_ubo *bad_out) {
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
  printf("UBO layout parameters@0 mvp@192 checker@256 srgb@268 size=%zu\n",
         sizeof(good));

  struct large_widget_ubo large_good = {.widget = good};
  struct large_widget_ubo large_bad = {.widget = bad};
  uint32_t ubo_bytes = kWidgetUboBytes;
  if (large) {
    for (unsigned i = 0; i < 4; ++i) large_good.widget.parameters[11][i] = 41 + i;
    for (unsigned m = 0; m < 14; ++m)
      for (unsigned i = 0; i < 16; ++i) large_good.matrices[m][i] = 1 + m * 16 + i;
    for (unsigned t = 0; t < 3; ++t)
      for (unsigned i = 0; i < 4; ++i) large_good.tail[t][i] = 51 + t + 10 * i;
    large_good.signed_tag = -37;
    large_good.enabled = 1;
    large_good.end_marker[0] = 91;
    large_good.end_marker[1] = 92;
    large_bad = large_good;
    large_bad.widget.srgbTarget = 0;
    large_bad.signed_tag = 37;
    large_bad.enabled = 0;
    ubo_bytes = sizeof(large_good);
    printf("UBO_LARGE matrices@272 array_stride=64 column_stride=16 tail@1168 "
           "signed@1216 bool@1220 end@1224 size=%u dynamic=%d\n", ubo_bytes, dynamic);
  }

  *good_out = large_good;
  *bad_out = large_bad;
  return ubo_bytes;
}

/* Distinct tags identify every descriptor's actual range. Unselected slots
 * retain a poison tag, with a valid identity matrix so mistakes remain visible. */
static void widget_multi_data(void *mapped, uint64_t stride,
                              const struct widget_ubo *good, const struct widget_ubo *bad) {
  memset(mapped, 0, (size_t)(stride * 11 + sizeof(*good)));
  const unsigned slots[4] = {3, 5, 7, 11};
  for (unsigned slot = 0; slot < 12; ++slot) {
    struct widget_ubo value = *good;
    value.parameters[1][0] = -1;
    for (unsigned i = 0; i < 4; ++i)
      if (slot == slots[i]) value.parameters[1][0] = 101 + i;
    if (slot == 6) { value = *bad; value.parameters[1][0] = 103; }
    memcpy((char *)mapped + stride * slot, &value, sizeof(value));
  }
}

#endif
