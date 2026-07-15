// mali_vk_bench.c
//
// Minimal Vulkan compute benchmark that talks DIRECTLY to libmali.so via
// dlopen/dlsym, bypassing the standard Vulkan loader (there isn't one on
// this TV). This works because Mali's driver blob exports the standard
// vkGetInstanceProcAddr entry point itself, which is enough to bootstrap
// everything else.
//
// Usage: ./mali_vk_bench /path/to/libmali.so
//
// Runs the wavelet-lifting-shaped compute shader (wavelet_bench.comp) at
// 2560x1440 and 3840x2160, N iterations each, and reports average GPU
// dispatch time in microseconds using timestamp queries.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include SHADER_HEADER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>

static PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr;

#define GET_IPROC(inst, name) (PFN_##name)pfn_vkGetInstanceProcAddr(inst, #name)
#define GET_DPROC(dev, get, name) (PFN_##name)get(dev, #name)

static void die(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(1);
}

static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "FATAL: %s failed with VkResult=%d\n", what, r);
        exit(1);
    }
}

typedef struct {
    uint32_t width;
    uint32_t height;
} PushConst;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/libmali.so\n", argv[0]);
        return 1;
    }

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) die(dlerror());

    pfn_vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr) {
        // Standard ICD interface entry point (what a real Vulkan loader
        // would call instead of the plain name). Same signature.
        pfn_vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    }
    if (!pfn_vkGetInstanceProcAddr) die("neither vkGetInstanceProcAddr nor vk_icdGetInstanceProcAddr found in libmali.so");

    PFN_vkCreateInstance vkCreateInstance =
        GET_IPROC(NULL, vkCreateInstance);
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion =
        GET_IPROC(NULL, vkEnumerateInstanceVersion);

    uint32_t apiVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&apiVersion);
    printf("Instance-level API version: %u.%u.%u\n",
           VK_API_VERSION_MAJOR(apiVersion),
           VK_API_VERSION_MINOR(apiVersion),
           VK_API_VERSION_PATCH(apiVersion));

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mali_vk_bench",
        .apiVersion = apiVersion,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };
    VkInstance instance;
    check(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");

    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        GET_IPROC(instance, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
        GET_IPROC(instance, vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
        GET_IPROC(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties =
        GET_IPROC(instance, vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCreateDevice vkCreateDevice =
        GET_IPROC(instance, vkCreateDevice);
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr =
        GET_IPROC(instance, vkGetDeviceProcAddr);

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, NULL);
    if (devCount == 0) die("no Vulkan physical devices found");
    VkPhysicalDevice *devices = malloc(sizeof(VkPhysicalDevice) * devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devices);
    VkPhysicalDevice phys = devices[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s (driver %u.%u.%u)\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.driverVersion),
           VK_API_VERSION_MINOR(props.driverVersion),
           VK_API_VERSION_PATCH(props.driverVersion));
    printf("timestampPeriod: %f ns/tick\n", props.limits.timestampPeriod);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, NULL);
    VkQueueFamilyProperties *qfs = malloc(sizeof(VkQueueFamilyProperties) * qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs);

    int computeFamily = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { computeFamily = (int)i; break; }
    }
    if (computeFamily < 0) die("no compute queue family found");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)computeFamily,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice device;
    check(vkCreateDevice(phys, &dci, NULL, &device), "vkCreateDevice");

    #define D(name) PFN_##name name = GET_DPROC(device, vkGetDeviceProcAddr, name)
    D(vkGetDeviceQueue);
    D(vkCreateBuffer);
    D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory);
    D(vkBindBufferMemory);
    D(vkMapMemory);
    D(vkUnmapMemory);
    D(vkCreateShaderModule);
    D(vkCreateDescriptorSetLayout);
    D(vkCreatePipelineLayout);
    D(vkCreateComputePipelines);
    D(vkCreateDescriptorPool);
    D(vkAllocateDescriptorSets);
    D(vkUpdateDescriptorSets);
    D(vkCreateCommandPool);
    D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer);
    D(vkCmdBindPipeline);
    D(vkCmdBindDescriptorSets);
    D(vkCmdPushConstants);
    D(vkCmdDispatch);
    D(vkCmdPipelineBarrier);
    D(vkCmdResetQueryPool);
    D(vkCmdWriteTimestamp);
    D(vkEndCommandBuffer);
    D(vkQueueSubmit);
    D(vkQueueWaitIdle);
    D(vkCreateQueryPool);
    D(vkGetQueryPoolResults);
    #undef D

    VkQueue queue;
    vkGetDeviceQueue(device, (uint32_t)computeFamily, 0, &queue);

    // ---- Buffer sized for the largest resolution we test (4K) ----
    const uint32_t MAX_W = 3840, MAX_H = 2160;
    VkDeviceSize bufSize = (VkDeviceSize)MAX_W * MAX_H * sizeof(float);

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bufSize,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    check(vkCreateBuffer(device, &bci, NULL, &buffer), "vkCreateBuffer");

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, buffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    int memType = -1;
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want) {
            memType = (int)i; break;
        }
    }
    if (memType < 0) die("no host-visible+coherent memory type found (unexpected on a UMA GPU)");

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReq.size,
        .memoryTypeIndex = (uint32_t)memType,
    };
    VkDeviceMemory mem;
    check(vkAllocateMemory(device, &mai, NULL, &mem), "vkAllocateMemory");
    check(vkBindBufferMemory(device, buffer, mem, 0), "vkBindBufferMemory");

    void *mapped;
    check(vkMapMemory(device, mem, 0, bufSize, 0, &mapped), "vkMapMemory");
    for (uint32_t i = 0; i < MAX_W * MAX_H; i++) ((float*)mapped)[i] = 0.5f;

    // ---- Shader / pipeline ----
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = wavelet_bench_spv_len * sizeof(uint32_t),
        .pCode = wavelet_bench_spv,
    };
    VkShaderModule shader;
    check(vkCreateShaderModule(device, &smci, NULL, &shader), "vkCreateShaderModule");

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    VkDescriptorSetLayout dsl;
    check(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl), "vkCreateDescriptorSetLayout");

    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(PushConst),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout pipelineLayout;
    check(vkCreatePipelineLayout(device, &plci, NULL, &pipelineLayout), "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shader,
        .pName = "main",
    };
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = pipelineLayout,
    };
    VkPipeline pipeline;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline),
          "vkCreateComputePipelines");

    VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
    };
    VkDescriptorPool descPool;
    check(vkCreateDescriptorPool(device, &dpci, NULL, &descPool), "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet descSet;
    check(vkAllocateDescriptorSets(device, &dsai, &descSet), "vkAllocateDescriptorSets");

    VkDescriptorBufferInfo dbi = { buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descSet,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbi,
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)computeFamily,
    };
    VkCommandPool cmdPool;
    check(vkCreateCommandPool(device, &cpci2, NULL, &cmdPool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(device, &cbai, &cmd), "vkAllocateCommandBuffers");

    const uint32_t N_ITERS = 200;
    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = N_ITERS * 2,
    };
    VkQueryPool queryPool;
    check(vkCreateQueryPool(device, &qpci, NULL, &queryPool), "vkCreateQueryPool");

    struct { const char *label; uint32_t w, h; } resolutions[] = {
        { "1440p (2560x1440)", 2560, 1440 },
        { "4K   (3840x2160)", 3840, 2160 },
    };

    for (int r = 0; r < 2; r++) {
        uint32_t w = resolutions[r].w, h = resolutions[r].h;
        PushConst pc = { w, h };

        VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        check(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer");
        vkCmdResetQueryPool(cmd, queryPool, 0, N_ITERS * 2);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout,
                                 0, 1, &descSet, 0, NULL);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

        uint32_t gx = (w + 7) / 8, gy = (h + 7) / 8;

        VkMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
        };

        for (uint32_t i = 0; i < N_ITERS; i++) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, i * 2);
            vkCmdDispatch(cmd, gx, gy, 1);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                  1, &barrier, 0, NULL, 0, NULL);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, i * 2 + 1);
        }
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
        };
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        clock_gettime(CLOCK_MONOTONIC, &t1);

        uint64_t *timestamps = malloc(sizeof(uint64_t) * N_ITERS * 2);
        check(vkGetQueryPoolResults(device, queryPool, 0, N_ITERS * 2,
                                     sizeof(uint64_t) * N_ITERS * 2, timestamps,
                                     sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "vkGetQueryPoolResults");

        double totalGpuNs = 0;
        for (uint32_t i = 0; i < N_ITERS; i++) {
            uint64_t delta = timestamps[i * 2 + 1] - timestamps[i * 2];
            totalGpuNs += (double)delta * props.limits.timestampPeriod;
        }
        double avgGpuUs = (totalGpuNs / N_ITERS) / 1000.0;
        double wallMs = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

        printf("\n%s\n", resolutions[r].label);
        printf("  avg GPU dispatch time: %.3f us\n", avgGpuUs);
        printf("  wall time for %u dispatches: %.2f ms (%.3f ms/dispatch incl. CPU overhead)\n",
               N_ITERS, wallMs, wallMs / N_ITERS);

        free(timestamps);
    }

    vkUnmapMemory(device, mem);
    printf("\nDone.\n");
    return 0;
}
