/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vulkan_context.h"

using namespace diagnostics;

namespace {
constexpr uint64_t timeout = 5'000'000'000;

template<class P> P device_proc(Context &c, const char *name)
{
    auto proc = reinterpret_cast<P>(c.gdpa(c.device, name));
    if (!proc) throw std::runtime_error(std::string("Missing ") + name);
    return proc;
}

struct Sync {
    Context &c;
    PFN_vkDestroySemaphore destroy;
    VkSemaphore timeline = VK_NULL_HANDLE, binary = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    explicit Sync(Context &c) : c(c), destroy(device_proc<PFN_vkDestroySemaphore>(c, "vkDestroySemaphore")) {}
    ~Sync() {
        if (!c.idle) return;
        if (fence) c.DestroyFence(c.device, fence, nullptr);
        if (binary) destroy(c.device, binary, nullptr);
        if (timeline) destroy(c.device, timeline, nullptr);
    }
};

std::string run(const char *directory, bool layer)
{
    unsetenv("HYBRIS_TIMELINE_SEMAPHORE");
    Context c;
    c.initialize(directory, layer);
    auto enumerate = c.instance_proc<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    uint32_t count = 0;
    CHECK(enumerate(c.physical, nullptr, &count, nullptr));
    std::vector<VkExtensionProperties> extensions(count);
    CHECK(enumerate(c.physical, nullptr, &count, extensions.data()));
    auto named = [](const auto &e) { return !strcmp(e.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME); };
    if (std::none_of(extensions.begin(), extensions.end(), named)) return "UNSUPPORTED: no timeline extension\n";

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
    c.instance_proc<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(c.physical, &features);
    if (!timeline.timelineSemaphore) return "UNSUPPORTED: timelineSemaphore=false\n";
    const char *enabled = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    c.create_device({}, reinterpret_cast<VkBaseOutStructure *>(&timeline), 1, &enabled);

    auto create = device_proc<PFN_vkCreateSemaphore>(c, "vkCreateSemaphore");
    auto counter = device_proc<PFN_vkGetSemaphoreCounterValueKHR>(c, "vkGetSemaphoreCounterValueKHR");
    auto signal = device_proc<PFN_vkSignalSemaphoreKHR>(c, "vkSignalSemaphoreKHR");
    auto wait = device_proc<PFN_vkWaitSemaphoresKHR>(c, "vkWaitSemaphoresKHR");
    auto fill = device_proc<PFN_vkCmdFillBuffer>(c, "vkCmdFillBuffer");
    Sync sync(c);
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr,
        VK_SEMAPHORE_TYPE_TIMELINE, 2};
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    CHECK(create(c.device, &semaphore, nullptr, &sync.timeline));
    semaphore.pNext = nullptr;
    CHECK(create(c.device, &semaphore, nullptr, &sync.binary));
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(c.CreateFence(c.device, &fence, nullptr, &sync.fence));

    uint64_t value = 0, target = 3;
    CHECK(counter(c.device, sync.timeline, &value));
    if (value != 2) throw std::runtime_error("Wrong initial value");
    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &sync.timeline, &target};
    if (wait(c.device, &wait_info, 0) != VK_TIMEOUT) throw std::runtime_error("Future value did not time out");
    VkSemaphoreSignalInfo signal_info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, sync.timeline, target};
    CHECK(signal(c.device, &signal_info));
    CHECK(wait(c.device, &wait_info, timeout));

    Buffer output(c, sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr, c.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    CHECK(c.AllocateCommandBuffers(c.device, &allocate, &command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(c.BeginCommandBuffer(command, &begin));
    constexpr uint32_t marker = 0x61aa375c;
    fill(command, output.handle, 0, sizeof(marker), marker);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, output.handle, 0, sizeof(marker)};
    c.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
    CHECK(c.EndCommandBuffer(command));

    VkSemaphore both[] = {sync.binary, sync.timeline};
    uint64_t values[] = {0, 5};
    VkTimelineSemaphoreSubmitInfo timeline_submit{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        nullptr, 0, nullptr, 2, values};
    const VkProtectedSubmitInfo immutable_prefix{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO, &timeline_submit, VK_FALSE};
    VkSubmitInfo first{VK_STRUCTURE_TYPE_SUBMIT_INFO, &immutable_prefix,
        0, nullptr, nullptr, 1, &command, 2, both};
    CHECK(c.QueueSubmit(c.queue, 1, &first, VK_NULL_HANDLE));
    c.idle = false;
    target = 5;
    CHECK(wait(c.device, &wait_info, timeout));
    CHECK(counter(c.device, sync.timeline, &value));
    if (value != 5) throw std::runtime_error("Wrong GPU signaled value");
    if (immutable_prefix.pNext != &timeline_submit) throw std::runtime_error("Caller submit chain changed");

    uint64_t final_value = 6;
    VkTimelineSemaphoreSubmitInfo second_timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        nullptr, 2, values, 1, &final_value};
    VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkSubmitInfo second{VK_STRUCTURE_TYPE_SUBMIT_INFO, &second_timeline, 2, both, stages,
        0, nullptr, 1, &sync.timeline};
    CHECK(c.QueueSubmit(c.queue, 1, &second, sync.fence));
    CHECK(c.WaitForFences(c.device, 1, &sync.fence, VK_TRUE, timeout));
    c.idle = true;
    CHECK(counter(c.device, sync.timeline, &value));
    output.visibility(false);
    if (value != final_value || *static_cast<const uint32_t *>(output.mapped) != marker)
        throw std::runtime_error("GPU signal or readback mismatch");
    c.FreeCommandBuffers(c.device, c.pool, 1, &command);
    return std::string("GPU: ") + c.properties.deviceName +
        "\nPASS: host signal, timeout, mixed binary/timeline submit, fence, and GPU readback\n";
}
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    std::string mode = argv[2];
    if (mode != "timeline-native" && mode != "timeline-compat") return 2;
    try {
        return report_result(run(argv[1], mode == "timeline-compat"));
    } catch (const std::exception &error) {
        return report_result(std::string("FAIL: ") + error.what() + "\n");
    }
}
