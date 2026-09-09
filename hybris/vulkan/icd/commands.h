/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HYBRIS_ICD_COMMANDS_H
#define HYBRIS_ICD_COMMANDS_H
#include "device.h"
int hybris_icd_command_device(VkCommandBuffer command, struct hybris_icd_device *device);
int hybris_icd_command_allocator(VkCommandBuffer command, VkAllocationCallbacks *allocator);
void hybris_icd_command_error(VkCommandBuffer command, VkResult error);
void hybris_icd_commands_release_device(VkDevice device);
PFN_vkVoidFunction hybris_icd_commands_proc(const char *name);
#endif
