// vk_1080p_single.c
//
// Minimal, single-shot comparison test: ONE 1080p forward transform chain
// (4 levels x 2 axes x 5 stages = 40 dispatches), using the proven staged
// shader (one barrier per dispatch). No loops, no repeated iterations —
// deliberately kept as small and low-risk as possible after the earlier
// hardware watchdog reset during a longer multi-iteration run.
//
// Usage: ./vk_1080p_single /path/to/libmali.so

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "dwt_shader_spv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

static PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr;
#define GET_IPROC(inst, name) (PFN_##name)pfn_vkGetInstanceProcAddr(inst, #name)
#define GET_DPROC(dev, get, name) (PFN_##name)get(dev, #name)

static void die(const char *msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(1); }
static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) { fprintf(stderr, "FATAL: %s failed VkResult=%d\n", what, r); exit(1); }
}

typedef struct { uint32_t direction, mode, stage, fullWidth, activeWidth, activeHeight; } PushConst;

#define N_LEVELS 4
#define N_STAGES 5
#define W 1920
#define H 1080

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s /path/to/libmali.so\n", argv[0]); return 1; }

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) die(dlerror());
    pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr)
        pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr) die("no Vulkan entry point found in libmali.so");

    PFN_vkCreateInstance vkCreateInstance = GET_IPROC(NULL, vkCreateInstance);
    VkApplicationInfo appInfo = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_1080p_single", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &appInfo };
    VkInstance instance;
    check(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");

    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = GET_IPROC(instance, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = GET_IPROC(instance, vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = GET_IPROC(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = GET_IPROC(instance, vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCreateDevice vkCreateDevice = GET_IPROC(instance, vkCreateDevice);
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = GET_IPROC(instance, vkGetDeviceProcAddr);

    uint32_t devCount = 1;
    VkPhysicalDevice phys;
    vkEnumeratePhysicalDevices(instance, &devCount, &phys);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, NULL);
    VkQueueFamilyProperties *qfs = malloc(sizeof(VkQueueFamilyProperties) * qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs);
    int computeFamily = -1;
    for (uint32_t i = 0; i < qfCount; i++)
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { computeFamily = (int)i; break; }
    if (computeFamily < 0) die("no compute queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)computeFamily, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice device;
    check(vkCreateDevice(phys, &dci, NULL, &device), "vkCreateDevice");

    #define D(name) PFN_##name name = GET_DPROC(device, vkGetDeviceProcAddr, name)
    D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory);
    D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout);
    D(vkCreateComputePipelines); D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets);
    D(vkUpdateDescriptorSets); D(vkCreateCommandPool); D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer); D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets);
    D(vkCmdPushConstants); D(vkCmdDispatch); D(vkCmdPipelineBarrier);
    D(vkCmdResetQueryPool); D(vkCmdWriteTimestamp); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkCreateQueryPool); D(vkGetQueryPoolResults);
    D(vkCreateFence); D(vkWaitForFences);
    #undef D

    VkQueue queue;
    vkGetDeviceQueue(device, (uint32_t)computeFamily, 0, &queue);

    VkDeviceSize bufSize = (VkDeviceSize)W * H * sizeof(float);
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    VkBuffer bufA, bufB;
    VkDeviceMemory memA, memB;
    void *mappedA, *mappedB;
    for (int b = 0; b < 2; b++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = bufSize, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        VkBuffer *buf = (b == 0) ? &bufA : &bufB;
        check(vkCreateBuffer(device, &bci, NULL, buf), "vkCreateBuffer");
        VkMemoryRequirements memReq;
        vkGetBufferMemoryRequirements(device, *buf, &memReq);
        int memType = -1;
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++)
            if ((memReq.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) { memType = (int)i; break; }
        if (memType < 0) die("no host-visible+coherent memory type");
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = memReq.size, .memoryTypeIndex = (uint32_t)memType };
        VkDeviceMemory *mem = (b == 0) ? &memA : &memB;
        check(vkAllocateMemory(device, &mai, NULL, mem), "vkAllocateMemory");
        check(vkBindBufferMemory(device, *buf, *mem, 0), "vkBindBufferMemory");
        void **mapped = (b == 0) ? &mappedA : &mappedB;
        check(vkMapMemory(device, *mem, 0, bufSize, 0, mapped), "vkMapMemory");
    }
    for (uint32_t i = 0; i < W * H; i++) ((float*)mappedA)[i] = 0.5f;
    for (uint32_t i = 0; i < W * H; i++) ((float*)mappedB)[i] = 0.5f;

    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = dwt_lifting_spv_len * sizeof(uint32_t), .pCode = dwt_lifting_spv };
    VkShaderModule shader;
    check(vkCreateShaderModule(device, &smci, NULL, &shader), "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings };
    VkDescriptorSetLayout dsl;
    check(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl), "vkCreateDescriptorSetLayout");

    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(PushConst) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pipelineLayout;
    check(vkCreatePipelineLayout(device, &plci, NULL, &pipelineLayout), "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" };
    VkComputePipelineCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = stage, .layout = pipelineLayout };
    VkPipeline pipeline;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline), "vkCreateComputePipelines");

    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &poolSize };
    VkDescriptorPool descPool;
    check(vkCreateDescriptorPool(device, &dpci, NULL, &descPool), "vkCreateDescriptorPool");

    VkDescriptorSetLayout layouts[2] = { dsl, dsl };
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descPool, .descriptorSetCount = 2, .pSetLayouts = layouts };
    VkDescriptorSet descSets[2];
    check(vkAllocateDescriptorSets(device, &dsai, descSets), "vkAllocateDescriptorSets");

    VkDescriptorBufferInfo dbiA = { bufA, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo dbiB = { bufB, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[4] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, descSets[0], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbiA, 0 },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, descSets[0], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbiB, 0 },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, descSets[1], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbiB, 0 },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, descSets[1], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbiA, 0 },
    };
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = (uint32_t)computeFamily };
    VkCommandPool cmdPool;
    check(vkCreateCommandPool(device, &cpci2, NULL, &cmdPool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    check(vkCreateFence(device, &fci, NULL, &fence), "vkCreateFence");

    VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    VkQueryPool queryPool;
    check(vkCreateQueryPool(device, &qpci, NULL, &queryPool), "vkCreateQueryPool");

    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };

    const uint32_t DISPATCHES = N_LEVELS * 2 * N_STAGES; // 40, forward only

    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    check(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer");
    vkCmdResetQueryPool(cmd, queryPool, 0, 2);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

    int pingpong = 0;
    for (uint32_t step = 0; step < DISPATCHES; step++) {
        uint32_t level = step / (2 * N_STAGES);
        uint32_t rem = step % (2 * N_STAGES);
        uint32_t axis = rem / N_STAGES;
        uint32_t stg = rem % N_STAGES;
        uint32_t activeW = W >> level, activeH = H >> level;

        PushConst pc = { axis, 0, stg, W, activeW, activeH };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descSets[pingpong], 0, NULL);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        uint32_t lineLen = (axis == 0) ? activeW : activeH;
        uint32_t numLines = (axis == 0) ? activeH : activeW;
        uint32_t gx = (lineLen + 255) / 256;
        vkCmdDispatch(cmd, gx, numLines, 1);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              0, 1, &barrier, 0, NULL, 0, NULL);
        pingpong ^= 1;
    }

    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    VkResult wr = vkWaitForFences(device, 1, &fence, VK_TRUE, 15ull * 1000 * 1000 * 1000);
    if (wr == VK_TIMEOUT) { printf("VULKAN: TIMED OUT after 15s\n"); return 1; }
    check(wr, "vkWaitForFences");

    uint64_t ts[2];
    check(vkGetQueryPoolResults(device, queryPool, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
          "vkGetQueryPoolResults");
    double us = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1000.0;
    printf("VULKAN 1080p single forward pass: %.3f us (%.3f ms)\n", us, us / 1000.0);

    return 0;
}
