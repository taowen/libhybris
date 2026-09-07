/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_SCALED_FIXTURE_H
#define HYBRIS_SCALED_FIXTURE_H
#include "shaders/scaled.vert.inc"
#include "shaders/scaled.multi.inc"
#include "shaders/scaled.literal.inc"
#include "shaders/scaled.frag.inc"
#include "shaders/scaled.matrix.inc"
#include "shaders/scaled.array.inc"
#include "shaders/scaled.nested.inc"
#include "shaders/scaled.matarray.inc"
#include "shaders/scaled.spec.inc"
#include "shaders/scaled.spec-direct.inc"
#include "shaders/scaled.group.inc"
#include "shaders/scaled.group-multi.inc"
#include "shaders/scaled.group-spec.inc"
static const struct scaled_case { VkFormat format; const char *name; unsigned bits, components, sign; } cases[] = {
#define CASE(n,b,c) {VK_FORMAT_##n##_USCALED, #n "_USCALED", b,c,0}, {VK_FORMAT_##n##_SSCALED, #n "_SSCALED",b,c,1}
 CASE(R8,8,1), CASE(R8G8,8,2), CASE(R8G8B8A8,8,4),
 CASE(R16,16,1), CASE(R16G16,16,2), CASE(R16G16B16A16,16,4)
#undef CASE
};

/* Longest matching mode names precede their shorter variants. */
static const struct scaled_shader {
  const char *mode;
  const uint32_t *code;
  size_t size;
  unsigned multiple, aggregate, specialized, direct;
} shaders[] = {
#define SHADER(mode, code, multi, aggregate, spec, direct) {mode, code, sizeof(code), multi, aggregate, spec, direct}
  SHADER("group-multi", kScaledGroupMultiSpv, 1, 0, 0, 0),
  SHADER("group-spec", kScaledGroupSpecSpv, 0, 1, 1, 0),
  SHADER("group", kScaledGroupSpv, 0, 0, 0, 0),
  SHADER("spec-direct", kScaledSpecDirectSpv, 0, 1, 1, 1),
  SHADER("spec", kScaledSpecSpv, 0, 1, 1, 0),
  SHADER("matarray", kScaledMatarraySpv, 0, 1, 0, 0),
  SHADER("matrix", kScaledMatrixSpv, 0, 1, 0, 0),
  SHADER("nested", kScaledNestedSpv, 0, 1, 0, 0),
  SHADER("array", kScaledArraySpv, 0, 1, 0, 0),
  SHADER("literal", kScaledLiteralSpv, 0, 0, 0, 0),
  SHADER("multi", kScaledMultiSpv, 1, 0, 0, 0),
  SHADER("", kScaledVertSpv, 0, 0, 0, 0),
#undef SHADER
};
#endif
