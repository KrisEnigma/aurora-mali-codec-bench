// dwt_bench.c
//
// Multi-level CDF 9/7 wavelet lifting benchmark on Mali via direct libmali.so
// dlopen (no system Vulkan loader on this TV, see mali_vk_bench.c for the
// bootstrap rationale). Times a 4-level forward (encode-equivalent) and
// inverse (decode-equivalent) transform chain separately, at 1080p, 1440p,
// and 3.5k (3584x2016). 4K intentionally not tested.
//
// Usage: ./dwt_bench /path/to/libmali.so

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include SHADER_HEADER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include <math.h>

static PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr;
#define GET_IPROC(inst, name) (PFN_##name)pfn_vkGetInstanceProcAddr(inst, #name)
#define GET_DPROC(dev, get, name) (PFN_##name)get(dev, #name)

static void die(const char *msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(1); }
static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS) { fprintf(stderr, "FATAL: %s failed VkResult=%d\n", what, r); exit(1); }
}

typedef struct {
    uint32_t direction;   // 0 = horizontal, 1 = vertical
    uint32_t mode;        // 0 = forward, 1 = inverse
    uint32_t fullWidth;
    uint32_t activeWidth;
    uint32_t activeHeight;
} PushConst;

#define N_LEVELS 4

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s /path/to/libmali.so\n", argv[0]); return 1; }

    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) die(dlerror());

    pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr)
        pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr) die("no Vulkan entry point found in libmali.so");

    PFN_vkCreateInstance vkCreateInstance = GET_IPROC(NULL, vkCreateInstance);
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = GET_IPROC(NULL, vkEnumerateInstanceVersion);

    uint32_t apiVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&apiVersion);

    VkApplicationInfo appInfo = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dwt_bench", .apiVersion = apiVersion };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &appInfo };
    VkInstance instance;
    check(vkCreateInstance(&ici, NULL, &instance), "vkCreateInstance");

    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = GET_IPROC(instance, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = GET_IPROC(instance, vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = GET_IPROC(instance, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = GET_IPROC(instance, vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCreateDevice vkCreateDevice = GET_IPROC(instance, vkCreateDevice);
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = GET_IPROC(instance, vkGetDeviceProcAddr);

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, NULL);
    if (devCount == 0) die("no Vulkan physical devices");
    VkPhysicalDevice *devices = malloc(sizeof(VkPhysicalDevice) * devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, devices);
    VkPhysicalDevice phys = devices[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s (driver %u.%u.%u), timestampPeriod=%f ns\n", props.deviceName,
           VK_API_VERSION_MAJOR(props.driverVersion), VK_API_VERSION_MINOR(props.driverVersion),
           VK_API_VERSION_PATCH(props.driverVersion), props.limits.timestampPeriod);

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
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice device;
    check(vkCreateDevice(phys, &dci, NULL, &device), "vkCreateDevice");

    #define D(name) PFN_##name name = GET_DPROC(device, vkGetDeviceProcAddr, name)
    D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkUnmapMemory);
    D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout);
    D(vkCreateComputePipelines); D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets);
    D(vkUpdateDescriptorSets); D(vkCreateCommandPool); D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer); D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets);
    D(vkCmdPushConstants); D(vkCmdDispatch); D(vkCmdPipelineBarrier);
    D(vkCmdResetQueryPool); D(vkCmdWriteTimestamp); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateQueryPool); D(vkGetQueryPoolResults);
    D(vkCreateFence); D(vkWaitForFences); D(vkResetFences);
    #undef D

    VkQueue queue;
    vkGetDeviceQueue(device, (uint32_t)computeFamily, 0, &queue);

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    check(vkCreateFence(device, &fci, NULL, &fence), "vkCreateFence");

    const uint32_t MAX_W = 3584, MAX_H = 2016; // sized for the largest test case (3.5k)
    VkDeviceSize bufSize = (VkDeviceSize)MAX_W * MAX_H * sizeof(float);

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

        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = memReq.size, .memoryTypeIndex = (uint32_t)memType };
        VkDeviceMemory *mem = (b == 0) ? &memA : &memB;
        check(vkAllocateMemory(device, &mai, NULL, mem), "vkAllocateMemory");
        check(vkBindBufferMemory(device, *buf, *mem, 0), "vkBindBufferMemory");

        void **mapped = (b == 0) ? &mappedA : &mappedB;
        check(vkMapMemory(device, *mem, 0, bufSize, 0, mapped), "vkMapMemory");
        for (uint32_t i = 0; i < MAX_W * MAX_H; i++) ((float*)*mapped)[i] = 0.5f;
    }

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
    VkDescriptorSet descSets[2]; // [0] = A->B, [1] = B->A
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

    const uint32_t N_ITERS = 15;
    const uint32_t DISPATCHES_PER_CHAIN = N_LEVELS * 2; // levels x (horizontal + vertical)
    VkQueryPoolCreateInfo qpci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = N_ITERS * 2 };
    VkQueryPool queryPool;
    check(vkCreateQueryPool(device, &qpci, NULL, &queryPool), "vkCreateQueryPool");

    VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };

    struct { const char *label; uint32_t w, h; } resolutions[] = {
        { "1080p (1920x1080)", 1920, 1080 },
        { "1440p (2560x1440)", 2560, 1440 },
        { "3.5k  (3584x2016)", 3584, 2016 },
    };

    // ---- Smoke test: verify the shader actually does the transform ----
    // Runs ONE forward horizontal pass on 16 known samples and compares
    // against a host-computed reference using identical math + boundary
    // handling, before trusting any timing numbers from the full chain.
    {
        const uint32_t N = 16;
        float initVal[16];
        for (uint32_t i = 0; i < N; i++) initVal[i] = 0.5f;
        memcpy(mappedA, initVal, sizeof(initVal));

        PushConst pc = { 0, 0, N, N, 1 }; // horizontal, forward, fullWidth=N, activeW=N, activeH=1
        VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        check(vkBeginCommandBuffer(cmd, &cbbi), "smoke vkBeginCommandBuffer");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descSets[0], 0, NULL);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, 1, 1, 1); // 16 samples fits in one workgroup of 256
        check(vkEndCommandBuffer(cmd), "smoke vkEndCommandBuffer");

        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        check(vkResetFences(device, 1, &fence), "smoke vkResetFences");
        check(vkQueueSubmit(queue, 1, &si, fence), "smoke vkQueueSubmit");
        VkResult wr = vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
        check(wr, "smoke vkWaitForFences");

        // Host-computed reference: identical math and clamped boundary
        // handling as the shader, run on plain floats.
        const float ALPHA_REF = -1.586134342f, BETA_REF = -0.05298011854f;
        const float GAMMA_REF = 0.8829110762f, DELTA_REF = 0.4435068522f, KAPPA_REF = 1.230174105f;
        float ref[16];
        memcpy(ref, initVal, sizeof(initVal));
        #define CLAMP16(x) ((x) < 0 ? 0 : ((x) >= (int)N ? (int)N - 1 : (x)))
        #define NEIGH(arr, idx, off) arr[CLAMP16((int)(idx) + (off))]
        for (uint32_t i = 1; i < N; i += 2) ref[i] = ref[i] + ALPHA_REF * (NEIGH(ref,i,-1) + NEIGH(ref,i,1));
        for (uint32_t i = 0; i < N; i += 2) ref[i] = ref[i] + BETA_REF  * (NEIGH(ref,i,-1) + NEIGH(ref,i,1));
        for (uint32_t i = 1; i < N; i += 2) ref[i] = ref[i] + GAMMA_REF * (NEIGH(ref,i,-1) + NEIGH(ref,i,1));
        for (uint32_t i = 0; i < N; i += 2) ref[i] = ref[i] + DELTA_REF * (NEIGH(ref,i,-1) + NEIGH(ref,i,1));
        for (uint32_t i = 0; i < N; i++) ref[i] = (i % 2 == 1) ? ref[i] * KAPPA_REF : ref[i] * (1.0f / KAPPA_REF);

        printf("Smoke test (1 forward horizontal pass, 16 samples, all init to 0.5):\n");
        printf("  GPU:  ");
        for (uint32_t i = 0; i < N; i++) printf("%.6f ", ((float*)mappedB)[i]);
        printf("\n  Host reference: ");
        for (uint32_t i = 0; i < N; i++) printf("%.6f ", ref[i]);
        printf("\n");
        int mismatch = 0;
        for (uint32_t i = 0; i < N; i++)
            if (fabsf(((float*)mappedB)[i] - ref[i]) > 1e-4f) mismatch = 1;
        printf("  -> %s\n\n", mismatch ? "MISMATCH: shader output does not match expected math" : "MATCH: shader is executing the transform correctly");

        // Reset buffers back to 0.5 for the main benchmark loop below.
        for (uint32_t i = 0; i < MAX_W * MAX_H; i++) ((float*)mappedA)[i] = 0.5f;
        for (uint32_t i = 0; i < MAX_W * MAX_H; i++) ((float*)mappedB)[i] = 0.5f;
    }

    for (int r = 0; r < 3; r++) {
        uint32_t fullW = resolutions[r].w, fullH = resolutions[r].h;

        printf("\n%s — per-level active dimensions: ", resolutions[r].label);
        for (int lvl = 0; lvl < N_LEVELS; lvl++)
            printf("L%d=%ux%u ", lvl, fullW >> lvl, fullH >> lvl);
        printf("\n");

        for (int passType = 0; passType < 2; passType++) { // 0 = forward chain, 1 = inverse chain
            printf("Running %s / %s ... ", resolutions[r].label,
                   passType == 0 ? "forward" : "inverse");
            fflush(stdout);

            VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            check(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer");
            vkCmdResetQueryPool(cmd, queryPool, 0, N_ITERS * 2);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

            for (uint32_t iter = 0; iter < N_ITERS; iter++) {
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, iter * 2);

                int pingpong = 0; // 0 = next dispatch reads A writes B, 1 = reads B writes A
                for (int step = 0; step < (int)DISPATCHES_PER_CHAIN; step++) {
                    int level, axis, mode;
                    uint32_t activeW, activeH;

                    if (passType == 0) {
                        level = step / 2;
                        axis = step % 2;
                        mode = 0;
                    } else {
                        int fstep = (int)DISPATCHES_PER_CHAIN - 1 - step;
                        level = fstep / 2;
                        axis = fstep % 2;
                        mode = 1;
                    }
                    activeW = fullW >> level;
                    activeH = fullH >> level;

                    PushConst pc = { (uint32_t)axis, (uint32_t)mode, fullW, activeW, activeH };
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

                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, iter * 2 + 1);
            }
            check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
            check(vkResetFences(device, 1, &fence), "vkResetFences");
            check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");

            const uint64_t TIMEOUT_NS = 30ull * 1000 * 1000 * 1000; // 30s
            VkResult waitResult = vkWaitForFences(device, 1, &fence, VK_TRUE, TIMEOUT_NS);
            if (waitResult == VK_TIMEOUT) {
                printf("TIMED OUT after 30s (likely a real GPU stall, not just slow progress)\n");
                continue; // skip result readback for this combo, move on
            }
            check(waitResult, "vkWaitForFences");

            uint64_t *timestamps = malloc(sizeof(uint64_t) * N_ITERS * 2);
            check(vkGetQueryPoolResults(device, queryPool, 0, N_ITERS * 2, sizeof(uint64_t) * N_ITERS * 2,
                                         timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                  "vkGetQueryPoolResults");

            double totalNs = 0;
            for (uint32_t i = 0; i < N_ITERS; i++)
                totalNs += (double)(timestamps[i*2+1] - timestamps[i*2]) * props.limits.timestampPeriod;
            double avgUs = (totalNs / N_ITERS) / 1000.0;

            printf("avg %.3f us (%.3f ms)\n", avgUs, avgUs / 1000.0);

            free(timestamps);
        }
    }

    printf("\nCorrectness sanity check (buffer started uniformly at 0.5):\n");
    printf("  bufA[0..7]: ");
    for (int i = 0; i < 8; i++) printf("%.6f ", ((float*)mappedA)[i]);
    printf("\n  bufB[0..7]: ");
    for (int i = 0; i < 8; i++) printf("%.6f ", ((float*)mappedB)[i]);
    printf("\n  (if these are still exactly 0.500000, the shader did not execute)\n");

    printf("\nDone.\n");
    return 0;
}
