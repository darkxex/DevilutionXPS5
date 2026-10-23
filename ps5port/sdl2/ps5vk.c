/*
 * DevilutionX for PS5 - the screen through Vulkan (RADV, VK_KHR_display on VideoOut).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RADV is linked into the title (a title cannot load a driver library). Its window system presents through
 * VK_KHR_display: a display-plane surface on the TV and a FIFO swapchain flipped at vblank. The swapchain's
 * images can only be colour attachments, so the picture is put on them by a small pipeline that samples a
 * texture (shaders/quad.*), not by a blit.
 *
 * The game draws in software. Each frame its picture goes into a mapped staging buffer and the GPU copies
 * it into the texture (the GPU does the tiling: glTexSubImage2D on the console's OpenGL did it on the CPU,
 * about 14 ns per pixel).
 */
#include "ps5vk.h"
#include "ps5vk_shaders.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES /* every command is a pointer looked up below */
#include <vulkan/vulkan.h>

/* RADV exports the ICD entry point; every command is looked up through it */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

extern int sceKernelDebugOutText(int channel, const char *text) __attribute__((weak));
/* not weak: a weak reference would not make the linker keep libSceSystemService (--as-needed) */
extern int sceSystemServiceHideSplashScreen(void);

#define FRAMES 2 /* frames in flight */
#define MAX_IMAGES 8
#define MAX_DRAWS 16
#define MAX_UPLOADS 16
#define MAX_TEXTURES 16

/* ---------------------------------------------------------------- functions */

#define VK_INSTANCE_FUNCS(X)                                                                       \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)          \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)           \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceDisplayPropertiesKHR)          \
    X(vkGetDisplayModePropertiesKHR) X(vkGetPhysicalDeviceDisplayPlanePropertiesKHR)             \
    X(vkGetDisplayPlaneSupportedDisplaysKHR) X(vkGetDisplayPlaneCapabilitiesKHR)                 \
    X(vkCreateDisplayPlaneSurfaceKHR) X(vkDestroySurfaceKHR)                                       \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)         \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)

#define VK_DEVICE_FUNCS(X)                                                                         \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR)           \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)                 \
    X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkCreateImageView) X(vkDestroyImageView)             \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)     \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateRenderPass)                     \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)                        \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout)                   \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)                 \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)     \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)             \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateCommandPool) X(vkDestroyCommandPool)        \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                    \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage)                    \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                           \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport)            \
    X(vkCmdSetScissor) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)    \
    X(vkCreateSemaphore) X(vkDestroySemaphore)

#define DECLARE(name) static PFN_##name name;
VK_INSTANCE_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance_;

/* ---------------------------------------------------------------- state */

struct PS5VKTexture
{
    int width, height, bgra;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set_linear, set_nearest;
    VkBuffer staging[FRAMES];
    VkDeviceMemory staging_memory[FRAMES];
    uint8_t *staging_map[FRAMES];
    int ready; /* the image holds a picture (its layout is then shader-read-only) */
};

typedef struct
{
    PS5VKTexture *texture;
    int x, y, w, h;
} Upload;

typedef struct
{
    PS5VKTexture *texture;
    float dst[4], uv[4];
    int linear;
} Draw;

static struct
{
    int open;
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    double hz;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkFramebuffer framebuffers[MAX_IMAGES];
    VkSemaphore rendered[MAX_IMAGES];
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkSampler linear, nearest;
    VkCommandPool commands;
    VkCommandBuffer cmd[FRAMES];
    VkFence done[FRAMES];
    VkSemaphore acquired[FRAMES];
    int frame;
    int frame_open; /* this frame's fence was waited for; uploads and draws are being collected */
    float clear[3];
    Upload uploads[MAX_UPLOADS];
    int upload_count;
    Draw draws[MAX_DRAWS];
    int draw_count;
} V;

static int want_width, want_height;
static char error_text[256];

/* Informational lines (the TV's modes, the swapchain, the first frames) only when built with -DPS5VK_DEBUG;
 * what a failure says is always sent. */
#ifdef PS5VK_DEBUG
#define VKLOG(...) ps5vk_log(__VA_ARGS__)
#else
#define VKLOG(...) ((void)0)
#endif

void ps5vk_log(const char *format, ...)
{
    char line[320];
    int n = snprintf(line, sizeof(line), "[DevilutionX vk] ");
    va_list args;
    va_start(args, format);
    vsnprintf(line + n, sizeof(line) - (size_t)n - 2, format, args);
    va_end(args);
    size_t length = strlen(line);
    line[length] = '\n';
    line[length + 1] = 0;
    if (sceKernelDebugOutText)
        sceKernelDebugOutText(0, line);
}

const char *ps5vk_error(void) { return error_text; }

void ps5vk_prefer_mode(int width, int height)
{
    want_width = width;
    want_height = height;
}

void ps5vk_size(int *width, int *height, double *hz)
{
    if (width)
        *width = (int)V.extent.width;
    if (height)
        *height = (int)V.extent.height;
    if (hz)
        *hz = V.hz;
}

#define FAIL(...)                                                                                  \
    do                                                                                             \
    {                                                                                              \
        snprintf(error_text, sizeof(error_text), __VA_ARGS__);                                     \
        ps5vk_log("%s", error_text);                                                               \
        return 0;                                                                                  \
    } while (0)

#define CHECK(call, what)                                                                          \
    do                                                                                             \
    {                                                                                              \
        VkResult check_result_ = (call);                                                           \
        if (check_result_ != VK_SUCCESS)                                                           \
            FAIL("%s failed (%d)", what, (int)check_result_);                                      \
    } while (0)

/* ---------------------------------------------------------------- helpers */

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < V.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (V.memory.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *out)
{
    int type = memory_type(req.memoryTypeBits, want);
    if (type < 0)
        return 0;
    VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = req.size;
    info.memoryTypeIndex = (uint32_t)type;
    return vkAllocateMemory(V.device, &info, NULL, out) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------------------------------------------------------- bring-up */

static int create_instance(void)
{
    vkCreateInstance_ = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance_)
        FAIL("the Vulkan driver has no vkCreateInstance");
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DevilutionX";
    app.pEngineName = "DevilutionX";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateInstance_(&info, NULL, &V.instance), "vkCreateInstance");
#define LOAD_INSTANCE(name)                                                                        \
    name = (PFN_##name)vk_icdGetInstanceProcAddr(V.instance, #name);                               \
    if (!name)                                                                                     \
        FAIL("missing %s", #name);
    VK_INSTANCE_FUNCS(LOAD_INSTANCE)
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(V.instance, &n, &V.gpu);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0)
        FAIL("no GPU");
    vkGetPhysicalDeviceMemoryProperties(V.gpu, &V.memory);
    return 1;
}

/* The TV: the mode closest to the one asked for at about 60 Hz, and a plane that can show it. */
static int create_surface(void)
{
    uint32_t n = 1;
    VkDisplayPropertiesKHR display;
    VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(V.gpu, &n, &display);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0)
        FAIL("no display");
    VkDisplayModePropertiesKHR modes[32];
    uint32_t mode_count = 32;
    vkGetDisplayModePropertiesKHR(V.gpu, display.display, &mode_count, modes);
    if (mode_count == 0)
        FAIL("no display modes");
    int best = -1;
    for (uint32_t i = 0; i < mode_count; ++i)
    {
        const VkDisplayModeParametersKHR *p = &modes[i].parameters;
        VKLOG("display mode %ux%u @ %.2f Hz", p->visibleRegion.width, p->visibleRegion.height,
                  p->refreshRate / 1000.0);
        int hz_ok = p->refreshRate >= 59000 && p->refreshRate <= 61000;
        if (!hz_ok)
            continue;
        if (want_width > 0 && (int)p->visibleRegion.width == want_width && (int)p->visibleRegion.height == want_height)
        {
            best = (int)i;
            break;
        }
        if (best < 0)
            best = (int)i;
    }
    if (best < 0)
        best = 0;
    const VkDisplayModePropertiesKHR *mode = &modes[best];

    VkDisplayPlanePropertiesKHR planes[8];
    uint32_t plane_count = 8;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(V.gpu, &plane_count, planes);
    uint32_t plane = 0;
    for (uint32_t i = 0; i < plane_count; ++i)
    {
        VkDisplayKHR supported[4];
        uint32_t count = 4;
        vkGetDisplayPlaneSupportedDisplaysKHR(V.gpu, i, &count, supported);
        int ok = 0;
        for (uint32_t k = 0; k < count; ++k)
            ok |= supported[k] == display.display;
        if (ok)
        {
            plane = i;
            break;
        }
    }
    VkDisplayPlaneCapabilitiesKHR caps;
    vkGetDisplayPlaneCapabilitiesKHR(V.gpu, mode->displayMode, plane, &caps);
    VkDisplayPlaneAlphaFlagBitsKHR alpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedAlpha & alpha))
        for (uint32_t bit = 1; bit <= VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR; bit <<= 1)
            if (caps.supportedAlpha & bit)
            {
                alpha = (VkDisplayPlaneAlphaFlagBitsKHR)bit;
                break;
            }

    VkDisplaySurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    info.displayMode = mode->displayMode;
    info.planeIndex = plane;
    info.planeStackIndex = plane < plane_count ? planes[plane].currentStackIndex : 0;
    info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.globalAlpha = 1.0f;
    info.alphaMode = alpha;
    info.imageExtent = mode->parameters.visibleRegion;
    CHECK(vkCreateDisplayPlaneSurfaceKHR(V.instance, &info, NULL, &V.surface), "vkCreateDisplayPlaneSurfaceKHR");
    V.hz = mode->parameters.refreshRate / 1000.0;
    VKLOG("using %ux%u @ %.2f Hz", mode->parameters.visibleRegion.width, mode->parameters.visibleRegion.height,
              V.hz);
    return 1;
}

static int create_device(void)
{
    VkQueueFamilyProperties families[8];
    uint32_t n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(V.gpu, &n, families);
    V.family = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(V.gpu, i, V.surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
        {
            V.family = i;
            break;
        }
    }
    if (V.family == UINT32_MAX)
        FAIL("no queue can draw and present");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = V.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(V.gpu, &info, NULL, &V.device), "vkCreateDevice");
#define LOAD_DEVICE(name)                                                                          \
    name = (PFN_##name)vkGetDeviceProcAddr(V.device, #name);                                       \
    if (!name)                                                                                     \
        FAIL("missing %s", #name);
    VK_DEVICE_FUNCS(LOAD_DEVICE)
    vkGetDeviceQueue(V.device, V.family, 0, &V.queue);
    return 1;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.gpu, V.surface, &caps), "surface capabilities");
    VkSurfaceFormatKHR formats[16];
    uint32_t n = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.gpu, V.surface, &n, formats);
    if (n == 0)
        FAIL("no surface formats");
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < n; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            chosen = formats[i];
            break;
        }
    V.format = chosen.format;
    V.extent = caps.currentExtent;
    if (V.extent.width == UINT32_MAX)
        FAIL("the surface has no size");
    uint32_t images = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && images > caps.maxImageCount)
        images = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = V.surface;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = V.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : (VkCompositeAlphaFlagBitsKHR)caps.supportedCompositeAlpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    CHECK(vkCreateSwapchainKHR(V.device, &info, NULL, &V.swapchain), "vkCreateSwapchainKHR");
    V.image_count = MAX_IMAGES;
    CHECK(vkGetSwapchainImagesKHR(V.device, V.swapchain, &V.image_count, V.images), "swapchain images");
    VKLOG("swapchain %ux%u, %u images, format %d", V.extent.width, V.extent.height, V.image_count, (int)V.format);
    return 1;
}

static int create_pipeline(void)
{
    VkAttachmentDescription color = {0};
    color.format = V.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    CHECK(vkCreateRenderPass(V.device, &rp, NULL, &V.pass), "vkCreateRenderPass");

    for (uint32_t i = 0; i < V.image_count; ++i)
    {
        VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = V.images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = V.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        CHECK(vkCreateImageView(V.device, &view, NULL, &V.views[i]), "swapchain view");
        VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = V.pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &V.views[i];
        fb.width = V.extent.width;
        fb.height = V.extent.height;
        fb.layers = 1;
        CHECK(vkCreateFramebuffer(V.device, &fb, NULL, &V.framebuffers[i]), "framebuffer");
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(vkCreateSemaphore(V.device, &sem, NULL, &V.rendered[i]), "semaphore");
    }

    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 1;
    dsl.pBindings = &binding;
    CHECK(vkCreateDescriptorSetLayout(V.device, &dsl, NULL, &V.set_layout), "set layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &V.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    CHECK(vkCreatePipelineLayout(V.device, &pl, NULL, &V.layout), "pipeline layout");

    VkShaderModule vert, frag;
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(SPV_QUAD_VERT);
    sm.pCode = SPV_QUAD_VERT;
    CHECK(vkCreateShaderModule(V.device, &sm, NULL, &vert), "vertex shader");
    sm.codeSize = sizeof(SPV_QUAD_FRAG);
    sm.pCode = SPV_QUAD_FRAG;
    CHECK(vkCreateShaderModule(V.device, &sm, NULL, &frag), "fragment shader");
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {0};
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = V.layout;
    gp.renderPass = V.pass;
    VkResult r = vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gp, NULL, &V.pipeline);
    vkDestroyShaderModule(V.device, vert, NULL);
    vkDestroyShaderModule(V.device, frag, NULL);
    CHECK(r, "vkCreateGraphicsPipelines");

    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(vkCreateSampler(V.device, &si, NULL, &V.linear), "sampler");
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    CHECK(vkCreateSampler(V.device, &si, NULL, &V.nearest), "nearest sampler");
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_TEXTURES};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 2 * MAX_TEXTURES;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    CHECK(vkCreateDescriptorPool(V.device, &dp, NULL, &V.pool), "descriptor pool");
    return 1;
}

static int create_frames(void)
{
    VkCommandPoolCreateInfo cp = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = V.family;
    CHECK(vkCreateCommandPool(V.device, &cp, NULL, &V.commands), "command pool");
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = V.commands;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = FRAMES;
    CHECK(vkAllocateCommandBuffers(V.device, &ca, V.cmd), "command buffers");
    for (int f = 0; f < FRAMES; ++f)
    {
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK(vkCreateFence(V.device, &fi, NULL, &V.done[f]), "fence");
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(vkCreateSemaphore(V.device, &sem, NULL, &V.acquired[f]), "semaphore");
    }
    return 1;
}

int ps5vk_open(void)
{
    if (V.open)
        return 0;
    memset(&V, 0, sizeof(V));
    error_text[0] = 0;
    /* the splash screen covers the display until the title asks for it to go */
    sceSystemServiceHideSplashScreen();
    if (!(create_instance() && create_surface() && create_device() && create_swapchain() && create_pipeline() &&
          create_frames()))
        return -1;
    V.open = 1;
    VKLOG("open");
    return 0;
}

void ps5vk_close(void)
{
    if (!V.open)
        return;
    vkDeviceWaitIdle(V.device);
    for (int f = 0; f < FRAMES; ++f)
    {
        vkDestroyFence(V.device, V.done[f], NULL);
        vkDestroySemaphore(V.device, V.acquired[f], NULL);
    }
    vkDestroyCommandPool(V.device, V.commands, NULL);
    vkDestroyDescriptorPool(V.device, V.pool, NULL);
    vkDestroySampler(V.device, V.linear, NULL);
    vkDestroySampler(V.device, V.nearest, NULL);
    vkDestroyPipeline(V.device, V.pipeline, NULL);
    vkDestroyPipelineLayout(V.device, V.layout, NULL);
    vkDestroyDescriptorSetLayout(V.device, V.set_layout, NULL);
    for (uint32_t i = 0; i < V.image_count; ++i)
    {
        vkDestroySemaphore(V.device, V.rendered[i], NULL);
        vkDestroyFramebuffer(V.device, V.framebuffers[i], NULL);
        vkDestroyImageView(V.device, V.views[i], NULL);
    }
    vkDestroyRenderPass(V.device, V.pass, NULL);
    vkDestroySwapchainKHR(V.device, V.swapchain, NULL);
    vkDestroyDevice(V.device, NULL);
    vkDestroySurfaceKHR(V.instance, V.surface, NULL);
    vkDestroyInstance(V.instance, NULL);
    V.open = 0;
}

/* ---------------------------------------------------------------- textures */

PS5VKTexture *ps5vk_texture_create(int width, int height, int bgra)
{
    if (!V.open || width <= 0 || height <= 0)
        return NULL;
    PS5VKTexture *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->width = width;
    t->height = height;
    t->bgra = bgra;
    VkFormat format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = (VkExtent3D){(uint32_t)width, (uint32_t)height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements req;
    if (vkCreateImage(V.device, &ii, NULL, &t->image) != VK_SUCCESS)
        goto fail;
    vkGetImageMemoryRequirements(V.device, t->image, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t->memory) ||
        vkBindImageMemory(V.device, t->image, t->memory, 0) != VK_SUCCESS)
        goto fail;
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = t->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(V.device, &view, NULL, &t->view) != VK_SUCCESS)
        goto fail;

    VkDescriptorSetLayout layouts[2] = {V.set_layout, V.set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = V.pool;
    da.descriptorSetCount = 2;
    da.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(V.device, &da, sets) != VK_SUCCESS)
        goto fail;
    t->set_linear = sets[0];
    t->set_nearest = sets[1];
    for (int k = 0; k < 2; ++k)
    {
        VkDescriptorImageInfo di = {k == 0 ? V.linear : V.nearest, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[k];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &di;
        vkUpdateDescriptorSets(V.device, 1, &w, 0, NULL);
    }

    for (int f = 0; f < FRAMES; ++f)
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (VkDeviceSize)width * height * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(V.device, &bi, NULL, &t->staging[f]) != VK_SUCCESS)
            goto fail;
        vkGetBufferMemoryRequirements(V.device, t->staging[f], &req);
        void *map = NULL;
        if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &t->staging_memory[f]) ||
            vkBindBufferMemory(V.device, t->staging[f], t->staging_memory[f], 0) != VK_SUCCESS ||
            vkMapMemory(V.device, t->staging_memory[f], 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
            goto fail;
        t->staging_map[f] = map;
        memset(map, 0, (size_t)width * height * 4);
    }
    VKLOG("texture %dx%d %s", width, height, bgra ? "bgra" : "rgba");
    return t;
fail:
    snprintf(error_text, sizeof(error_text), "texture %dx%d could not be made", width, height);
    ps5vk_log("%s", error_text);
    ps5vk_texture_destroy(t);
    return NULL;
}

void ps5vk_texture_destroy(PS5VKTexture *t)
{
    if (!t)
        return;
    if (V.open)
    {
        vkDeviceWaitIdle(V.device);
        for (int f = 0; f < FRAMES; ++f)
        {
            if (t->staging[f])
                vkDestroyBuffer(V.device, t->staging[f], NULL);
            if (t->staging_memory[f])
                vkFreeMemory(V.device, t->staging_memory[f], NULL);
        }
        if (t->view)
            vkDestroyImageView(V.device, t->view, NULL);
        if (t->image)
            vkDestroyImage(V.device, t->image, NULL);
        if (t->memory)
            vkFreeMemory(V.device, t->memory, NULL);
    }
    /* a queued upload or draw of it must not outlive it */
    for (int i = 0; i < V.upload_count; ++i)
        if (V.uploads[i].texture == t)
            V.uploads[i].texture = NULL;
    for (int i = 0; i < V.draw_count; ++i)
        if (V.draws[i].texture == t)
            V.draws[i].texture = NULL;
    free(t);
}

/* The frame in the making: wait for the fence of the frame whose staging buffers are about to be written. */
static void frame_begin(void)
{
    if (V.frame_open)
        return;
    vkWaitForFences(V.device, 1, &V.done[V.frame], VK_TRUE, UINT64_MAX);
    V.frame_open = 1;
    V.upload_count = 0;
    V.draw_count = 0;
    V.clear[0] = V.clear[1] = V.clear[2] = 0.0f;
}

void ps5vk_texture_update(PS5VKTexture *t, int x, int y, int width, int height, const void *pixels, int pitch)
{
    if (!V.open || !t || width <= 0 || height <= 0 || x < 0 || y < 0 || x + width > t->width || y + height > t->height)
        return;
    frame_begin();
    /* a second update of the same rectangle in one frame reuses its upload record */
    uint8_t *base = t->staging_map[V.frame];
    const size_t row = (size_t)width * 4;
    for (int r = 0; r < height; ++r)
        memcpy(base + ((size_t)(y + r) * t->width + (size_t)x) * 4, (const uint8_t *)pixels + (size_t)r * pitch, row);
    for (int i = 0; i < V.upload_count; ++i)
    {
        Upload *u = &V.uploads[i];
        if (u->texture == t)
        {
            /* grow the record to the union of both rectangles */
            int x1 = u->x + u->w > x + width ? u->x + u->w : x + width;
            int y1 = u->y + u->h > y + height ? u->y + u->h : y + height;
            u->x = u->x < x ? u->x : x;
            u->y = u->y < y ? u->y : y;
            u->w = x1 - u->x;
            u->h = y1 - u->y;
            return;
        }
    }
    if (V.upload_count < MAX_UPLOADS)
        V.uploads[V.upload_count++] = (Upload){t, x, y, width, height};
}

/* ---------------------------------------------------------------- frames */

void ps5vk_frame_clear(float r, float g, float b)
{
    if (!V.open)
        return;
    frame_begin();
    V.clear[0] = r;
    V.clear[1] = g;
    V.clear[2] = b;
}

void ps5vk_frame_draw(PS5VKTexture *t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                      int linear)
{
    if (!V.open || !t || V.draw_count >= MAX_DRAWS)
        return;
    frame_begin();
    Draw *d = &V.draws[V.draw_count++];
    d->texture = t;
    /* screen pixels to clip space, y down */
    d->dst[0] = 2.0f * x0 / (float)V.extent.width - 1.0f;
    d->dst[1] = 2.0f * y0 / (float)V.extent.height - 1.0f;
    d->dst[2] = 2.0f * x1 / (float)V.extent.width - 1.0f;
    d->dst[3] = 2.0f * y1 / (float)V.extent.height - 1.0f;
    d->uv[0] = u0;
    d->uv[1] = v0;
    d->uv[2] = u1;
    d->uv[3] = v1;
    d->linear = linear;
}

void ps5vk_frame_present(void)
{
    if (!V.open)
        return;
    frame_begin();
    const int f = V.frame;

    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(V.device, V.swapchain, UINT64_MAX, V.acquired[f], VK_NULL_HANDLE, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        static int warned;
        if (!warned++)
            ps5vk_log("acquire failed (%d)", (int)r);
        V.frame_open = 0;
        return;
    }
    vkResetFences(V.device, 1, &V.done[f]);

    VkCommandBuffer cb = V.cmd[f];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    for (int i = 0; i < V.upload_count; ++i)
    {
        Upload *u = &V.uploads[i];
        PS5VKTexture *t = u->texture;
        if (!t)
            continue;
        barrier(cb, t->image, t->ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0};
        copy.bufferOffset = ((VkDeviceSize)u->y * t->width + (VkDeviceSize)u->x) * 4;
        copy.bufferRowLength = (uint32_t)t->width;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageOffset = (VkOffset3D){u->x, u->y, 0};
        copy.imageExtent = (VkExtent3D){(uint32_t)u->w, (uint32_t)u->h, 1};
        vkCmdCopyBufferToImage(cb, t->staging[f], t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        t->ready = 1;
    }

    VkClearValue clear = {{{V.clear[0], V.clear[1], V.clear[2], 1.0f}}};
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = V.pass;
    rp.framebuffer = V.framebuffers[index];
    rp.renderArea.extent = V.extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0, 0, (float)V.extent.width, (float)V.extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, V.extent};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipeline);
    for (int i = 0; i < V.draw_count; ++i)
    {
        Draw *d = &V.draws[i];
        if (!d->texture || !d->texture->ready)
            continue;
        VkDescriptorSet set = d->linear ? d->texture->set_linear : d->texture->set_nearest;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &set, 0, NULL);
        float push[8] = {d->dst[0], d->dst[1], d->dst[2], d->dst[3], d->uv[0], d->uv[1], d->uv[2], d->uv[3]};
        vkCmdPushConstants(cb, V.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
        vkCmdDraw(cb, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &V.acquired[f];
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &V.rendered[index];
    VkResult submitted = vkQueueSubmit(V.queue, 1, &submit, V.done[f]);

    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &V.rendered[index];
    present.swapchainCount = 1;
    present.pSwapchains = &V.swapchain;
    present.pImageIndices = &index;
    VkResult presented = vkQueuePresentKHR(V.queue, &present);
    static unsigned frames;
    if (frames < 5 || frames % 600 == 0)
        VKLOG("frame %u: acquire %d, submit %d, present %d; uploads %d, draws %d, clear %.2f %.2f %.2f", frames,
                  (int)r, (int)submitted, (int)presented, V.upload_count, V.draw_count, V.clear[0], V.clear[1],
                  V.clear[2]);
    ++frames;

    V.frame = (V.frame + 1) % FRAMES;
    V.frame_open = 0;
}
