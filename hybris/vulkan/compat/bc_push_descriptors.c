/* SPDX-License-Identifier: Apache-2.0
 * Native push-descriptor recording integrated with the BC state journal.
 * Templates are expanded at recording time so their handles and caller-owned
 * data need not survive command-buffer recording. No driver capability is
 * invented: this path is exposed only when the native extension is present.
 */
#define VK_NO_PROTOTYPES
#include "bc_context.h"
#include <stdlib.h>
#include <string.h>

#define PROC(name) PFN_vk##name name = (PFN_vk##name)device->resolver(device->handle, "vk" #name)
struct hybris_bc_push_template {
    VkDescriptorUpdateTemplate handle;
    VkPipelineBindPoint point;
    uint32_t count;
    struct hybris_bc_push_template *next;
    VkDescriptorUpdateTemplateEntry entries[];
};
static size_t element_size(VkDescriptorType type)
{
    switch(type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return sizeof(VkDescriptorImageInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return sizeof(VkDescriptorBufferInfo);
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return sizeof(VkBufferView);
    default: return 0;
    }
}
static void VKAPI_CALL push(VkCommandBuffer handle, VkPipelineBindPoint point,
    VkPipelineLayout layout, uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    PROC(CmdPushDescriptorSetKHR);
    CmdPushDescriptorSetKHR(handle, point, layout, set, count, writes);
    hybris_bc_save_pushed_descriptors(&command->state, point, layout, set, count, writes);
}
static void push2(VkCommandBuffer handle, const VkPushDescriptorSetInfoKHR *info, const char *name)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command = hybris_bc_command_find(handle, &device);
    if (!command) return;
    ((PFN_vkCmdPushDescriptorSet2KHR)device->resolver(device->handle,name))(handle,info);
    if (info->pNext || !info->layout) { command->state.error=VK_ERROR_FEATURE_NOT_PRESENT; return; }
    if (info->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        hybris_bc_save_pushed_descriptors(&command->state,VK_PIPELINE_BIND_POINT_COMPUTE,
            info->layout,info->set,info->descriptorWriteCount,info->pDescriptorWrites);
    if (info->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        hybris_bc_save_pushed_descriptors(&command->state,VK_PIPELINE_BIND_POINT_GRAPHICS,
            info->layout,info->set,info->descriptorWriteCount,info->pDescriptorWrites);
}
static void VKAPI_CALL push2_khr(VkCommandBuffer handle,const VkPushDescriptorSetInfoKHR *info)
{push2(handle,info,"vkCmdPushDescriptorSet2KHR");}
static void VKAPI_CALL push2_core(VkCommandBuffer handle,const VkPushDescriptorSetInfoKHR *info)
{push2(handle,info,"vkCmdPushDescriptorSet2");}

static VkResult create_template(VkDevice handle,const VkDescriptorUpdateTemplateCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkDescriptorUpdateTemplate *out,const char *name)
{
    struct hybris_bc_device *device=hybris_bc_device_find(handle);
    if (!device) return VK_ERROR_INITIALIZATION_FAILED;
    struct hybris_bc_push_template *record=NULL;
    if (info->templateType==VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) {
        if (info->descriptorUpdateEntryCount > (SIZE_MAX-sizeof(*record))/sizeof(*record->entries))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        record=calloc(1,sizeof(*record)+(size_t)info->descriptorUpdateEntryCount*sizeof(*record->entries));
        if (!record) return VK_ERROR_OUT_OF_HOST_MEMORY;
        record->point=info->pipelineBindPoint; record->count=info->descriptorUpdateEntryCount;
        for(uint32_t i=0;i<record->count;++i) {
            record->entries[i]=info->pDescriptorUpdateEntries[i];
            size_t size=element_size(record->entries[i].descriptorType);
            size_t count=record->entries[i].descriptorCount, offset=record->entries[i].offset;
            if (!size || (count && (offset>SIZE_MAX-size ||
                (count>1 && record->entries[i].stride>(SIZE_MAX-offset-size)/(count-1))))) {
                free(record);return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
    }
    VkResult result=((PFN_vkCreateDescriptorUpdateTemplate)device->resolver(handle,name))(handle,info,allocator,out);
    if(result!=VK_SUCCESS) {free(record);return result;}
    if(record) {
        record->handle=*out;
        pthread_mutex_lock(&device->guard);
        record->next=device->push_templates;device->push_templates=record;
        pthread_mutex_unlock(&device->guard);
    }
    return result;
}
static VkResult VKAPI_CALL create_core(VkDevice handle,const VkDescriptorUpdateTemplateCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkDescriptorUpdateTemplate *out)
{return create_template(handle,info,allocator,out,"vkCreateDescriptorUpdateTemplate");}
static VkResult VKAPI_CALL create_khr(VkDevice handle,const VkDescriptorUpdateTemplateCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkDescriptorUpdateTemplate *out)
{return create_template(handle,info,allocator,out,"vkCreateDescriptorUpdateTemplateKHR");}
static void destroy_template(VkDevice handle,VkDescriptorUpdateTemplate tmpl,const VkAllocationCallbacks *allocator,const char *name)
{
    struct hybris_bc_device *device=hybris_bc_device_find(handle);
    if(!device)return;
    pthread_mutex_lock(&device->guard);
    for(struct hybris_bc_push_template **slot=&device->push_templates;*slot;slot=&(*slot)->next)
        if((*slot)->handle==tmpl) {struct hybris_bc_push_template *old=*slot;*slot=old->next;free(old);break;}
    pthread_mutex_unlock(&device->guard);
    ((PFN_vkDestroyDescriptorUpdateTemplate)device->resolver(handle,name))(handle,tmpl,allocator);
}
static void VKAPI_CALL destroy_core(VkDevice handle,VkDescriptorUpdateTemplate tmpl,const VkAllocationCallbacks *allocator)
{destroy_template(handle,tmpl,allocator,"vkDestroyDescriptorUpdateTemplate");}
static void VKAPI_CALL destroy_khr(VkDevice handle,VkDescriptorUpdateTemplate tmpl,const VkAllocationCallbacks *allocator)
{destroy_template(handle,tmpl,allocator,"vkDestroyDescriptorUpdateTemplateKHR");}

static void VKAPI_CALL push_template(VkCommandBuffer handle,VkDescriptorUpdateTemplate tmpl,
    VkPipelineLayout layout,uint32_t set,const void *data)
{
    struct hybris_bc_device *device;
    struct hybris_bc_command *command=hybris_bc_command_find(handle,&device);
    if(!command)return;
    pthread_mutex_lock(&device->guard);
    struct hybris_bc_push_template *record=device->push_templates;
    while(record && record->handle!=tmpl)record=record->next;
    if(!record) {pthread_mutex_unlock(&device->guard);command->state.error=VK_ERROR_INITIALIZATION_FAILED;return;}
    /* Each entry is expanded independently: descriptor arrays may have gaps or
     * stride zero. Neither template data nor metadata is retained by recording. */
    uint32_t count=record->count;
    VkPipelineBindPoint point=record->point;
    VkWriteDescriptorSet *writes=calloc(count?count:1,sizeof(*writes));
    if(!writes) {pthread_mutex_unlock(&device->guard);command->state.error=VK_ERROR_OUT_OF_HOST_MEMORY;return;}
    for(uint32_t i=0;i<count;++i) {
        const VkDescriptorUpdateTemplateEntry *e=&record->entries[i];
        size_t size=element_size(e->descriptorType);
        if(e->descriptorCount>SIZE_MAX/size) {command->state.error=VK_ERROR_OUT_OF_HOST_MEMORY;break;}
        unsigned char *payload=malloc(e->descriptorCount?(size_t)e->descriptorCount*size:1);
        if(!payload) {command->state.error=VK_ERROR_OUT_OF_HOST_MEMORY;break;}
        for(uint32_t j=0;j<e->descriptorCount;++j)
            memcpy(payload+(size_t)j*size,(const unsigned char*)data+e->offset+(size_t)j*e->stride,size);
        writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding=e->dstBinding,.dstArrayElement=e->dstArrayElement,
            .descriptorCount=e->descriptorCount,.descriptorType=e->descriptorType};
        switch(e->descriptorType) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: writes[i].pBufferInfo=(void*)payload;break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: writes[i].pTexelBufferView=(void*)payload;break;
        default: writes[i].pImageInfo=(void*)payload;break;
        }
    }
    pthread_mutex_unlock(&device->guard);
    if(command->state.error==VK_SUCCESS)push(handle,point,layout,set,count,writes);
    for(uint32_t i=0;i<count;++i)free((void*)(writes[i].pImageInfo?(const void*)writes[i].pImageInfo:
        writes[i].pBufferInfo?(const void*)writes[i].pBufferInfo:(const void*)writes[i].pTexelBufferView));
    free(writes);
}
static void VKAPI_CALL push_template2(VkCommandBuffer handle,const VkPushDescriptorSetWithTemplateInfoKHR *info)
{
    if(info->pNext || !info->layout) {
        struct hybris_bc_device *device;
        struct hybris_bc_command *command=hybris_bc_command_find(handle,&device);
        if(command)command->state.error=VK_ERROR_FEATURE_NOT_PRESENT;
        return;
    }
    push_template(handle,info->descriptorUpdateTemplate,info->layout,info->set,info->pData);
}
void hybris_bc_push_templates_finish(struct hybris_bc_device *device)
{
    while(device->push_templates) {
        struct hybris_bc_push_template *next=device->push_templates->next;
        free(device->push_templates);device->push_templates=next;
    }
}
PFN_vkVoidFunction hybris_bc_push_descriptors_proc(const char *name)
{
    static const struct {const char *name;PFN_vkVoidFunction proc;} entries[]={
#define ENTRY(name,proc) {"vk" #name,(PFN_vkVoidFunction)proc}
        ENTRY(CmdPushDescriptorSetKHR,push),ENTRY(CmdPushDescriptorSet,push),
        ENTRY(CmdPushDescriptorSet2KHR,push2_khr),ENTRY(CmdPushDescriptorSet2,push2_core),
        ENTRY(CreateDescriptorUpdateTemplate,create_core),ENTRY(CreateDescriptorUpdateTemplateKHR,create_khr),
        ENTRY(DestroyDescriptorUpdateTemplate,destroy_core),ENTRY(DestroyDescriptorUpdateTemplateKHR,destroy_khr),
        ENTRY(CmdPushDescriptorSetWithTemplateKHR,push_template),ENTRY(CmdPushDescriptorSetWithTemplate,push_template),
        ENTRY(CmdPushDescriptorSetWithTemplate2KHR,push_template2),ENTRY(CmdPushDescriptorSetWithTemplate2,push_template2)
#undef ENTRY
    };
    for(size_t i=0;i<sizeof(entries)/sizeof(*entries);++i)if(!strcmp(name,entries[i].name))return entries[i].proc;
    return NULL;
}
