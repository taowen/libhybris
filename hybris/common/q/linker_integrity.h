/* Integrity of a module after recorded, loader-owned TLS relocations. */
#pragma once
#include <stdint.h>
#include <vector>
struct HybrisTlsPatch {
  uintptr_t address;
  uint32_t original;
  uint32_t replacement;
};
struct soinfo;
bool hybris_relocate_module_integrity(soinfo *si, const std::vector<HybrisTlsPatch>& patches);
