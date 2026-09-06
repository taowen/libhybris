/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_MALI_QUIRKS_H
#define HYBRIS_MALI_QUIRKS_H
__attribute__((visibility("hidden")))
void *hybris_mali_hook(const char *symbol, const char *requester);
#endif
