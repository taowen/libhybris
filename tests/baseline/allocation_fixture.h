/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_ALLOCATION_FIXTURE_H
#define HYBRIS_ALLOCATION_FIXTURE_H
struct allocation_probe { unsigned live, calls, fail_after; int reject; };
struct allocation_header { void *base; size_t size; };

static void *VKAPI_CALL instance_allocate(void *user, size_t size, size_t alignment,
                                         VkSystemAllocationScope scope) {
  (void)scope;
  struct allocation_probe *p = user;
  unsigned call = __atomic_add_fetch(&p->calls, 1, __ATOMIC_RELAXED);
  if (p->reject || (p->fail_after && call > p->fail_after)) return NULL;
  if (alignment < _Alignof(struct allocation_header)) alignment = _Alignof(struct allocation_header);
  if (!alignment || (alignment & (alignment - 1)) ||
      size > SIZE_MAX - sizeof(struct allocation_header) - (alignment - 1)) return NULL;
  void *base = malloc(size + sizeof(struct allocation_header) + alignment - 1);
  if (!base) return NULL;
  uintptr_t address = ((uintptr_t)base + sizeof(struct allocation_header) + alignment - 1) & ~(alignment - 1);
  struct allocation_header *h = (struct allocation_header *)address - 1;
  h->base = base;
  h->size = size;
  __atomic_add_fetch(&p->live, 1, __ATOMIC_RELAXED);
  return (void *)address;
}

static void VKAPI_CALL instance_free(void *user, void *memory) {
  if (!memory) return;
  struct allocation_probe *p = user;
  struct allocation_header *h = (struct allocation_header *)memory - 1;
  free(h->base);
  __atomic_sub_fetch(&p->live, 1, __ATOMIC_RELAXED);
}

static void *VKAPI_CALL instance_reallocate(void *user, void *original, size_t size,
    size_t alignment, VkSystemAllocationScope scope) {
  if (!size) { instance_free(user, original); return NULL; }
  void *replacement = instance_allocate(user, size, alignment, scope);
  if (replacement && original) {
    size_t old_size = ((struct allocation_header *)original - 1)->size;
    memcpy(replacement, original, old_size < size ? old_size : size);
    instance_free(user, original);
  }
  return replacement;
}

#endif
