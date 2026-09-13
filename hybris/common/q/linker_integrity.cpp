/*
 * Preserve BoringSSL's actual integrity test across loader-owned TLS rewrites.
 * Verify the original image before updating its expected checksum. No function
 * is replaced, no constructor is skipped, and the on-disk library is unchanged.
 * This is integrity for a transformed image, not a claim of FIPS validation.
 */
#include "linker_integrity.h"
#include "integrity_sha256.h"
#include "linker_soinfo.h"
#include "linker_debug.h"
#include "linker_globals.h"
#include "../tls_patcher.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

namespace {
struct Range {
  uintptr_t begin;
  size_t size;
  Range(uintptr_t address = 0, size_t length = 0) : begin(address), size(length) {}
};

bool overlaps(Range a, Range b) {
  return a.begin < b.begin + b.size && b.begin < a.begin + a.size;
}

// Metadata may name only file-backed, read-only bytes of this particular DSO.
bool mapped_range(soinfo* si, Range range, uint32_t required, uint32_t forbidden) {
  if (!range.size || range.begin > UINTPTR_MAX - range.size) return false;
  for (size_t i = 0; i < si->phnum; ++i) {
    const auto& ph = si->phdr[i];
    if (ph.p_type != PT_LOAD || (ph.p_flags & required) != required ||
        (ph.p_flags & forbidden) || ph.p_vaddr > UINTPTR_MAX - si->load_bias) continue;
    const uintptr_t begin = si->load_bias + ph.p_vaddr;
    if (range.begin >= begin && range.begin - begin <= ph.p_filesz &&
        range.size <= ph.p_filesz - (range.begin - begin)) return true;
  }
  return false;
}

const ElfW(Sym)* symbol(soinfo* si, const char* name) {
  SymbolName lookup(name);
  const ElfW(Sym)* result = nullptr;
  if (!si->find_symbol_by_name(lookup, nullptr, &result) || !result ||
      result->st_shndx == SHN_UNDEF || result->st_shndx == SHN_ABS ||
      result->st_value > UINTPTR_MAX - si->load_bias) return nullptr;
  return result;
}
uintptr_t address(soinfo* si, const ElfW(Sym)* sym) {
  return sym ? si->load_bias + sym->st_value : 0;
}

// BoringSSL's shared-module format hashes LE64(length), text,
// LE64(length), rodata with HMAC-SHA256 and a fixed all-zero key.
// Static-module format has one combined text/rodata range without lengths.
class ModuleHmac {
 public:
  ModuleHmac() {
    unsigned char pad[64];
    memset(pad, 0x36, sizeof(pad));
    hybris_sha256_init(&inner_);
    hybris_sha256_update(&inner_, pad, sizeof(pad));
  }
  void bytes(const void* data, size_t size) {
    hybris_sha256_update(&inner_, data, size);
  }
  void length(size_t size) {
    unsigned char encoded[8];
    uint64_t value = size;
    for (unsigned i = 0; i < 8; ++i) encoded[i] = value >> (8 * i);
    bytes(encoded, sizeof(encoded));
  }
  void finish(unsigned char out[32]) {
    unsigned char digest[32], pad[64];
    hybris_sha256_out(&inner_, digest);
    memset(pad, 0x5c, sizeof(pad));
    hybris_sha256_context outer;
    hybris_sha256_init(&outer);
    hybris_sha256_update(&outer, pad, sizeof(pad));
    hybris_sha256_update(&outer, digest, sizeof(digest));
    hybris_sha256_out(&outer, out);
  }
 private:
  hybris_sha256_context inner_;
};

bool hash_range(ModuleHmac& hash, Range range,
                const std::vector<HybrisTlsPatch>& patches, bool original) {
  uintptr_t cursor = range.begin;
  const uintptr_t end = range.begin + range.size;
  for (const auto& patch : patches) {
    if (!overlaps(range, {patch.address, sizeof(uint32_t)})) continue;
    if (patch.address < cursor || end - patch.address < sizeof(uint32_t)) return false;
    hash.bytes(reinterpret_cast<const void*>(cursor), patch.address - cursor);
    const uint32_t word = original ? patch.original : patch.replacement;
    hash.bytes(&word, sizeof(word));
    cursor = patch.address + sizeof(word);
  }
  hash.bytes(reinterpret_cast<const void*>(cursor), end - cursor);
  return true;
}
bool hash_module(Range text, Range rodata, const std::vector<HybrisTlsPatch>& patches,
                 bool original, unsigned char out[32]) {
  ModuleHmac hash;
  if (rodata.size) hash.length(text.size);
  if (!hash_range(hash, text, patches, original)) return false;
  if (rodata.size) {
    hash.length(rodata.size);
    if (!hash_range(hash, rodata, patches, original)) return false;
  }
  hash.finish(out);
  return true;
}
bool equal_hash(const unsigned char* a, const unsigned char* b) {
  unsigned difference = 0;
  for (unsigned i = 0; i < 32; ++i) difference |= a[i] ^ b[i];
  return difference == 0;
}

// Older BoringSSL exports region bounds but no hash accessor. Resolve by the
// full verified content digest, never by a build-specific byte signature or
// instruction offset. Only one match in separate read-only data is acceptable.
// A corrupt input produces no match; ambiguity is an error. The native check
// still runs and is the final authority on which digest the module consumes.
const unsigned char* legacy_expected_hash(soinfo* si, Range text, Range rodata,
                                         const unsigned char original[32]) {
  const unsigned char* found = nullptr;
  for (size_t i = 0; i < si->phnum; ++i) {
    const auto& ph = si->phdr[i];
    if (ph.p_type != PT_LOAD || ph.p_flags != PF_R || ph.p_filesz < 32 ||
        ph.p_vaddr > UINTPTR_MAX - si->load_bias) continue;
    Range data{si->load_bias + ph.p_vaddr, static_cast<size_t>(ph.p_filesz)};
    if (!mapped_range(si, data, PF_R, PF_W | PF_X)) continue;
    for (uintptr_t cursor = data.begin; cursor <= data.begin + data.size - 32; ++cursor) {
      auto candidate = reinterpret_cast<const unsigned char*>(cursor);
      if (candidate[0] != original[0] || !equal_hash(candidate, original)) continue;
      Range hash{cursor, 32};
      if (overlaps(hash, text) || (rodata.size && overlaps(hash, rodata))) continue;
      if (found) return nullptr;
      found = candidate;
    }
  }
  return found;
}
bool error(soinfo* si, const char* message) {
  snprintf(linker_get_error_buffer(), linker_get_error_buffer_size(),
           "module integrity: %s: %s", si->get_realpath(), message);
  DL_ERR("module integrity: %s: %s", si->get_realpath(), message);
  return false;
}
}

bool hybris_relocate_module_integrity(soinfo* si, const std::vector<HybrisTlsPatch>& journal) {
  if (journal.empty()) return true;
  const auto start = symbol(si, "BORINGSSL_bcm_text_start");
  const auto end = symbol(si, "BORINGSSL_bcm_text_end");
  const auto getter = symbol(si, "FIPS_module_hash");
  const auto check = symbol(si, "BORINGSSL_integrity_test");
  if (!start && !end && !getter && !check) return true;
  if (!start || !end)
    return error(si, "TLS-rewritten module lacks exported integrity metadata");
  if (getter && (ELF64_ST_TYPE(getter->st_info) != STT_FUNC ||
      !mapped_range(si, {address(si, getter), getter->st_size}, PF_X, PF_W)))
    return error(si, "invalid FIPS_module_hash accessor");

  const uintptr_t text_start = address(si, start), text_end = address(si, end);
  if (text_end <= text_start) return error(si, "invalid protected text bounds");
  const Range text{text_start, text_end - text_start};
  if (!mapped_range(si, text, PF_X, PF_W)) return error(si, "protected text is not read-only module code");

  Range rodata;
  const auto ro_start = symbol(si, "BORINGSSL_bcm_rodata_start");
  const auto ro_end = symbol(si, "BORINGSSL_bcm_rodata_end");
  if (!!ro_start != !!ro_end) return error(si, "incomplete protected rodata bounds");
  if (ro_start) {
    const uintptr_t begin = address(si, ro_start), end = address(si, ro_end);
    if (end <= begin) return error(si, "invalid protected rodata bounds");
    rodata = {begin, end - begin};
    if (!mapped_range(si, rodata, PF_R, PF_W) || overlaps(text, rodata))
      return error(si, "invalid protected rodata mapping");
  }

  auto patches = journal;
  std::sort(patches.begin(), patches.end(), [](const HybrisTlsPatch& a, const HybrisTlsPatch& b) {
    return a.address < b.address;
  });
  uintptr_t previous_end = 0;
  for (const auto& patch : patches) {
    if (!mapped_range(si, {patch.address, sizeof(uint32_t)}, PF_X, PF_W) ||
        patch.address < previous_end || !hybris_is_tls_mrs(patch.original) ||
        (patch.replacement & 0xfc000000U) != 0x14000000U)
      return error(si, "invalid TLS relocation journal");
    uint32_t actual;
    memcpy(&actual, reinterpret_cast<const void*>(patch.address), sizeof(actual));
    if (actual != patch.replacement) return error(si, "TLS relocation changed after patching");
    previous_end = patch.address + sizeof(uint32_t);
  }

  unsigned char original[32], relocated[32];
  if (!hash_module(text, rodata, patches, true, original) ||
      !hash_module(text, rodata, patches, false, relocated))
    return error(si, "TLS relocation crosses integrity-region bounds");

  // Prefer the public pure pointer accessor. Older releases are identified
  // by a unique full digest in read-only data, with the same fail-closed rule.
  const unsigned char* expected;
  if (getter) {
    auto get_hash = reinterpret_cast<const unsigned char* (*)(void)>(address(si, getter));
    expected = get_hash();
  } else {
    expected = legacy_expected_hash(si, text, rodata, original);
    if (!expected) return error(si, "original module HMAC missing or ambiguous in legacy module");
  }
  const Range hash{reinterpret_cast<uintptr_t>(expected), 32};
  if (!mapped_range(si, hash, PF_R, PF_W | PF_X) || overlaps(hash, text) ||
      (rodata.size && overlaps(hash, rodata)))
    return error(si, "expected hash is not separate read-only module data");

  if (!equal_hash(original, expected))
    return error(si, "original module HMAC mismatch; refusing to re-sign modified input");
  if (equal_hash(relocated, expected)) return true;

  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || (page_size & (page_size - 1))) return error(si, "invalid page size");
  const uintptr_t first = hash.begin & ~(static_cast<uintptr_t>(page_size) - 1);
  if (hash.begin + hash.size > UINTPTR_MAX - static_cast<uintptr_t>(page_size - 1))
    return error(si, "hash page overflow");
  const uintptr_t last = (hash.begin + hash.size + page_size - 1) & ~(static_cast<uintptr_t>(page_size) - 1);
  for (size_t i = 0; i < si->phnum; ++i) {
    const auto& ph = si->phdr[i];
    if (ph.p_type != PT_LOAD || !(ph.p_flags & (PF_W | PF_X))) continue;
    if (ph.p_vaddr > UINTPTR_MAX - si->load_bias ||
        ph.p_memsz > UINTPTR_MAX - (si->load_bias + ph.p_vaddr))
      return error(si, "invalid neighboring segment bounds");
    if (overlaps({first, last - first}, {si->load_bias + ph.p_vaddr, ph.p_memsz}))
      return error(si, "expected-hash page shares writable or executable data");
  }
  void* page = reinterpret_cast<void*>(first);
  if (mprotect(page, last - first, PROT_READ | PROT_WRITE))
    return error(si, "cannot update private expected-hash page");
  memcpy(const_cast<unsigned char*>(expected), relocated, sizeof(relocated));
  if (mprotect(page, last - first, PROT_READ))
    return error(si, "cannot restore expected-hash page protection");
  fprintf(stderr, "HYBRIS: verified original module and relocated integrity hash for %s\n", si->get_realpath());
  return true;
}
