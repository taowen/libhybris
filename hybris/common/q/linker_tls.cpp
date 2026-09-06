/*
 * Copyright (C) 2019 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "linker_tls.h"

#include <vector>

#include <android/versioning.h>
#include "async_safe/CHECK.h"
#include "private/ScopedRWLock.h"
#include "private/ScopedSignalBlocker.h"
#include "private/bionic_defs.h"
#include "private/bionic_elf_tls.h"
#include "private/bionic_globals.h"
#include "private/linker_native_bridge.h"
#include "linker_main.h"
#include "linker_soinfo.h"
#include "tls_patcher.h"
#include <stdint.h>

// Set by linker_main.cpp from android_linker_init(); we read its
// init_static_tls_for_thread to push .tdata into the promoting thread's
// tls_static_tls (and to register the module for catch-up replay on
// later-touched threads -- see hooks.c::g_promoted_tls).
extern hybris_tls_patcher_funcs_t _tls_patcher_funcs;

__LIBC_HIDDEN__ _Atomic(size_t) __libc_tls_generation_copy = {SIZE_MAX};

static bool g_static_tls_finished;
static std::vector<TlsModule> g_tls_modules;

static size_t get_unused_module_index() {
  for (size_t i = 0; i < g_tls_modules.size(); ++i) {
    // Only recycle slots that unregistered cleanly back to dynamic — never
    // reuse a slot whose static_offset still holds a real reservation.
    // unregister_tls_module()'s static path leaves static_offset intact so
    // any live thread DTV's `static_tls + offset` pointer at this slot
    // stays valid; if we recycled and a future register dropped the slot
    // back to SIZE_MAX, that pointer would route through the dynamic-free
    // path in update_tls_dtv() / __free_dynamic_tls() and be passed to
    // BionicAllocator::free — heap corruption (the pointer is into TCB
    // static-TLS memory, not allocator-owned).
    if (g_tls_modules[i].soinfo_ptr == nullptr &&
        g_tls_modules[i].static_offset == SIZE_MAX) {
      return i;
    }
  }
  g_tls_modules.push_back({});
  __libc_shared_globals()->tls_modules.module_count = g_tls_modules.size();
  __libc_shared_globals()->tls_modules.module_table = g_tls_modules.data();
  return g_tls_modules.size() - 1;
}

static void register_tls_module(soinfo* si, size_t static_offset) {
  TlsModules& libc_modules = __libc_shared_globals()->tls_modules;

  // The global TLS module table points at the std::vector of modules declared
  // in this file, so acquire a write lock before modifying the std::vector.
  ScopedSignalBlocker ssb;
  ScopedWriteLock locker(&libc_modules.rwlock);

  size_t module_idx = get_unused_module_index();

  soinfo_tls* si_tls = si->get_tls();
  si_tls->module_id = __tls_module_idx_to_id(module_idx);

  const size_t new_generation = ++libc_modules.generation;
  __libc_tls_generation_copy = new_generation;
  if (libc_modules.generation_libc_so != nullptr) {
    *libc_modules.generation_libc_so = new_generation;
  }

  g_tls_modules[module_idx].segment = si_tls->segment;
  g_tls_modules[module_idx].static_offset = static_offset;
  g_tls_modules[module_idx].first_generation = new_generation;
  g_tls_modules[module_idx].soinfo_ptr = si;
}

static void unregister_tls_module(soinfo* si) {
  TlsModules& libc_modules = __libc_shared_globals()->tls_modules;

  ScopedSignalBlocker ssb;
  ScopedWriteLock locker(&libc_modules.rwlock);

  soinfo_tls* si_tls = si->get_tls();
  TlsModule& mod = g_tls_modules[__tls_module_id_to_idx(si_tls->module_id)];
  CHECK(mod.soinfo_ptr == si);

  // No generation bump. Upstream bionic doesn't bump here either: the
  // signal "this slot died" is `mod = {}` setting first_generation =
  // kTlsGenerationNone (dynamic path) or static_offset staying != SIZE_MAX
  // (static path); update_tls_dtv reads those, not libc_modules.generation.
  // Under libhybris there's no consumer at all -- bionic TLS access goes
  // through the thunk-patched MRS, not __tls_get_addr -> update_tls_dtv.
  if (mod.static_offset == SIZE_MAX) {
    // Dynamic: clean fast path. Slot fully recycles in
    // get_unused_module_index() (gated on static_offset == SIZE_MAX).
    mod = {};
  } else {
    // Promoted to static at IE relocation time. Detach the soinfo and
    // mark the slot permanent (get_unused_module_index() will skip it),
    // but keep static_offset/segment/first_generation intact so:
    //   - update_tls_dtv() keeps treating dtv->modules[i] = static_tls + off
    //     for any thread that still has a stale DTV; reading stale (but
    //     mapped) static-TLS bytes is harmless. Recycling and flipping
    //     back to SIZE_MAX would route a static_tls pointer through the
    //     dynamic-free path — see get_unused_module_index() comment.
    //   - __free_dynamic_tls() at thread exit takes its
    //     "static_offset != SIZE_MAX -> skip free" branch (bionic_elf_tls.cpp).
    // Cost: the slot index AND the reserved bytes in static_tls_layout
    // leak. In libhybris this only fires for IE-model libs (apex libc et
    // al.), which transitively unload at most once per process and are
    // typically pinned for life.
    mod.soinfo_ptr = nullptr;
  }
  si_tls->module_id = kTlsUninitializedModuleId;
}

// The reference is valid until a TLS module is registered, unregistered,
// or promoted (promote_tls_module_to_static() mutates the entry under
// rwlock; callers without the lock should copy fields by value, not
// retain the reference).
const TlsModule& get_tls_module(size_t module_id) {
  size_t module_idx = __tls_module_id_to_idx(module_id);
  CHECK(module_idx < g_tls_modules.size());
  return g_tls_modules[module_idx];
}

extern "C" void __linker_reserve_bionic_tls_in_static_tls() {
  __libc_shared_globals()->static_tls_layout.reserve_bionic_tls();
}

void linker_setup_exe_static_tls(const char* progname) {
  soinfo* somain = solist_get_somain();
  StaticTlsLayout& layout = __libc_shared_globals()->static_tls_layout;
  if (somain->get_tls() == nullptr) {
    layout.reserve_exe_segment_and_tcb(nullptr, progname);
  } else {
    register_tls_module(somain, layout.reserve_exe_segment_and_tcb(&somain->get_tls()->segment, progname));
  }

  // The pthread key data is located at the very front of bionic_tls. As a
  // temporary workaround, allocate bionic_tls just after the thread pointer so
  // Golang can find its pthread key, as long as the executable's TLS segment is
  // small enough. Specifically, Golang scans forward 384 words from the TP on
  // ARM.
  //  - http://b/118381796
  //  - https://github.com/golang/go/issues/29674
  __linker_reserve_bionic_tls_in_static_tls();
}

void linker_finalize_static_tls() {
  g_static_tls_finished = true;
  __libc_shared_globals()->static_tls_layout.finish_layout();
}

void register_soinfo_tls(soinfo* si) {
  soinfo_tls* si_tls = si->get_tls();
  if (si_tls == nullptr || si_tls->module_id != kTlsUninitializedModuleId) {
    return;
  }
  // Reserve bionic_tcb at offset 0 of the static-TLS layout on the first
  // TLS-using soinfo we ever see, before any link_image() in this batch
  // calls relocate(). This keeps offset_thread_pointer() stable from
  // that moment on, so soinfo::relocate can read it into tls_tp_base
  // unconditionally at the top of the function. Mirrors what
  // linker_setup_exe_static_tls would do for an exe; libhybris has none.
  // The reservation holds tls_modules.rwlock briefly, then drops it
  // before register_tls_module re-takes it. The gap is safe because
  // the outer g_dl_mutex (held by every hybris_dlopen path that reaches
  // here) already serialises mutation of the static_tls_layout.
  StaticTlsLayout& layout = __libc_shared_globals()->static_tls_layout;
  if (layout.offset_bionic_tcb() == SIZE_MAX) {
    ScopedSignalBlocker ssb;
    ScopedWriteLock locker(&__libc_shared_globals()->tls_modules.rwlock);
    layout.reserve_exe_segment_and_tcb(nullptr, "");
  }
  // Always register as dynamic (SIZE_MAX). In standard bionic this branch
  // would reserve a static-TLS slot for executable bootstrap deps before
  // linker_finalize_static_tls() flips g_static_tls_finished. libhybris has
  // no executable, so finalize is never called and EVERY load would otherwise
  // claim a static slot it can never give back. Instead, the IE TLS
  // relocation path (linker.cpp:R_GENERIC_TLS_TPREL) lazy-promotes via
  // promote_tls_module_to_static() the few libs that actually need a fixed
  // tp-relative offset (apex libc et al.). Everything else stays dynamic
  // and dlclose() recycles the slot cleanly.
  register_tls_module(si, SIZE_MAX);
}

size_t promote_tls_module_to_static(soinfo* si) {
  ScopedSignalBlocker ssb;
  ScopedWriteLock locker(&__libc_shared_globals()->tls_modules.rwlock);

  // We're growing the static-TLS layout. linker_finalize_static_tls() is
  // never called in the libhybris flow today (linker_main is dead code),
  // but if a future change wires it up, reserving after finalize would
  // silently corrupt the layout — make it loud instead.
  CHECK(!g_static_tls_finished);

  soinfo_tls* si_tls = si->get_tls();
  CHECK(si_tls != nullptr);
  CHECK(si_tls->module_id != kTlsUninitializedModuleId);
  TlsModule& mod = g_tls_modules[__tls_module_id_to_idx(si_tls->module_id)];
  CHECK(mod.soinfo_ptr == si);
  if (mod.static_offset == SIZE_MAX) {
    StaticTlsLayout& layout = __libc_shared_globals()->static_tls_layout;
    mod.static_offset = layout.reserve_solib_segment(si_tls->segment);
    // Bump generation so threads update their DTV from "dynamic, unallocated"
    // to "static at this offset" on next access.
    TlsModules& libc_modules = __libc_shared_globals()->tls_modules;
    const size_t new_generation = ++libc_modules.generation;
    __libc_tls_generation_copy = new_generation;
    if (libc_modules.generation_libc_so != nullptr) {
      *libc_modules.generation_libc_so = new_generation;
    }
    mod.first_generation = new_generation;

    // Hand the (offset, .tdata copy, sizes) tuple to hooks.c: it
    // memcpy's .tdata into the promoting thread's tls_static_tls and
    // appends an entry to its registry so any thread that later
    // first-touches bionic state via _hybris_hook___get_tls_hooks can
    // replay the init. hooks owns the .tdata bytes after this call --
    // segment.init_ptr stays valid only while the .so is mapped.
    if (_tls_patcher_funcs.init_static_tls_for_thread != nullptr) {
      _tls_patcher_funcs.init_static_tls_for_thread(
          mod.static_offset,
          si_tls->segment.init_ptr,
          si_tls->segment.init_size,
          si_tls->segment.size);
    }
  }
  return mod.static_offset;
}

void unregister_soinfo_tls(soinfo* si) {
  soinfo_tls* si_tls = si->get_tls();
  if (si_tls == nullptr || si_tls->module_id == kTlsUninitializedModuleId) {
    return;
  }
  return unregister_tls_module(si);
}

extern "C" {
extern __attribute__((visibility("hidden"))) void *(*hybris_tlsdesc_get_tls)(void);
__attribute__((visibility("hidden"))) size_t hybris_tlsdesc_static_offset(const size_t *descriptor) {
  CHECK(hybris_tlsdesc_get_tls != nullptr);
  hybris_tlsdesc_get_tls();
  return descriptor[1];
}
}
