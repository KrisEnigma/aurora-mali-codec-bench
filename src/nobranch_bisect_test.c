// tile_bisect_test.c
//
// Same structural pattern as dwt_lifting.comp (two-buffer descriptor set,
// shared-memory tile + halo load, one barrier) but trivial unconditional
// math (x*2+1) instead of the multi-stage CDF 9/7 lifting. Isolates
// whether the bug is in this shared structure or specifically in the
// lifting shader's conditional multi-stage logic.
//
// Usage: ./tile_bisect_test /path/to/libmali.so

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include SHADER_HEADER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <math.h>

static PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr;
#define GET_IPROC(inst, name) (PFN_##name)pfn_vkGetInstanceProcAddr(inst, #name)
#define GET_DPROC(dev, get, name) (PFN_##name)get(dev, #name)

static void die(const char *msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(1); }
static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) { fprintf(stderr, "FATAL: %s failed VkResult=%d\n", what, r); exit(1); }
}

typedef struct {
    uint32_t direction, mode, fullWidth, activeWidth, activeHeight;
} PushConst;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s /path/to/libmali.so\n", argv[0]); return 1; }

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) die(dlerror());
    pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr)
        pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr) die("no Vulkan entry point found in libmali.so");

    PFN_vkCreateInstance vkCreateInstance = GET_IPROC(NULL, vkCreateInstance);
    VkApplicationInfo appInfo = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "tile_bisect", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &appInfo };
    VkInstance instance;
    check(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");

    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = GET_IPROC(instance, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = GET_IPROC(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = GET_IPROC(instance, vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCreateDevice vkCreateDevice = GET_IPROC(instance, vkCreateDevice);
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = GET_IPROC(instance, vkGetDeviceProcAddr);

    uint32_t devCount = 1;
    VkPhysicalDevice phys;
    vkEnumeratePhysicalDevices(instance, &devCount, &phys);

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
    D(vkCmdPushConstants); D(vkCmdDispatch); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkCreateFence); D(vkWaitForFences);
    #undef D

    VkQueue queue;
    vkGetDeviceQueue(device, (uint32_t)computeFamily, 0, &queue);

    const uint32_t N = 16;
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    VkBuffer bufA, bufB;
    VkDeviceMemory memA, memB;
    void *mappedA, *mappedB;
    for (int b = 0; b < 2; b++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = N * sizeof(float), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
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
        check(vkMapMemory(device, *mem, 0, N * sizeof(float), 0, mapped), "vkMapMemory");
    }
    for (uint32_t i = 0; i < N; i++) ((float*)mappedA)[i] = 0.5f;
    for (uint32_t i = 0; i < N; i++) ((float*)mappedB)[i] = -1.0f; // distinct sentinel so we know if dst was ever touched

    VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = nobranch_bisect_spv_len * sizeof(uint32_t), .pCode = nobranch_bisect_spv };
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

    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &poolSize };
    VkDescriptorPool descPool;
    check(vkCreateDescriptorPool(device, &dpci, NULL, &descPool), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descPool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet descSet;
    check(vkAllocateDescriptorSets(device, &dsai, &descSet), "vkAllocateDescriptorSets");

    VkDescriptorBufferInfo dbiA = { bufA, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo dbiB = { bufB, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, descSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbiA, 0 },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, descSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbiB, 0 },
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

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

    PushConst pc = { 0, 0, N, N, 1 };
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    check(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descSet, 0, NULL);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, 1, 1, 1);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "vkWaitForFences");

    printf("Src (bufA, unchanged, should still be 0.5): ");
    for (uint32_t i = 0; i < N; i++) printf("%.6f ", ((float*)mappedA)[i]);
    printf("\nDst (bufB) GPU output:                      ");
    for (uint32_t i = 0; i < N; i++) printf("%.6f ", ((float*)mappedB)[i]);
    printf("\n");

    // Same reference values already validated by hand in the earlier smoke
    // test (constant input -> near-0.5 approx / near-0 detail interior,
    // boundary effects at the edges from clamped neighbor access).
    float ref[16] = {
        0.596398f, -0.091272f, 0.473251f, 0.000000f, 0.500000f, 0.000000f, 0.500000f, 0.000000f,
        0.500000f, 0.000000f, 0.500000f, 0.000000f, 0.500000f, 0.000000f, -0.041516f, -1.847755f
    };
    printf("Expected (from earlier validated reference):");
    for (uint32_t i = 0; i < N; i++) printf(" %.6f", ref[i]);
    printf("\n");

    int pass = 1;
    for (uint32_t i = 0; i < N; i++) if (fabsf(((float*)mappedB)[i] - ref[i]) > 1e-3f) pass = 0;
    printf("-> %s\n", pass ? "PASS: matches expected lifting math without the outer mode-branch wrapper" :
                              "FAIL: still wrong even without the outer mode-branch, bug is in the multi-barrier/conditional pattern itself");

    return 0;
}
