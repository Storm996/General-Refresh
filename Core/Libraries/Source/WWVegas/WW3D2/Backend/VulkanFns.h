// GeneralsX @refactor Qoder 18/10/2026 pure-Vulkan wedge 5.0: shared Vulkan loader surface.
//
// Why this file exists. Wedges 1-4 all lived in one translation unit (Backend/VulkanCore.cpp) because
// the entry-point table, the core singleton and the queue lock were file-static there. Wedge 5 starts
// splitting the draw path (PSO cache + state replay) and wedges 6-8 add fixed-function vertex/pixel
// stages and the shader registry, which cannot all fit in one 2400-line TU. Those new TUs need the
// same function pointers, so the table moves here and VulkanCore.cpp keeps the single instance.
//
// Contents:
//  - VKWP1_LOG: release-visible stderr logging (AGENTS.md rule: DEBUG_LOG is compiled out).
//  - SAGE_VKFN_LIST + struct Vkfns + the one global instance G (declared here, defined in
//    VulkanCore.cpp) plus the import helper imp() and the VkResult guard chk().
//  - vk_queue_lock()/vk_queue_unlock(): the process-wide critical section that serialises
//    vkQueueSubmit, the shared command pool, the descriptor pool and fence waits. Defined in
//    VulkanCore.cpp, declared here so every Vulkan TU shares one lock.
//
// The list carries no comments on purpose: a // inside a backslash-continued macro is spliced into one
// logical line first and would then swallow the rest of the list. There is no
// vkGetPhysicalDeviceLimits entry point - limits ride inside VkPhysicalDeviceProperties.
//
// Windows hazards recorded the hard way, keep them in mind when adding to this file:
//  - 'small' is a macro (rpcndr.h defines it as 'char' and windows.h drags that in): never use it as
//    an identifier.
//  - Vulkan handle types are distinct structs, so chained assignment (a = b = VK_NULL_HANDLE) between
//    two different handle kinds does not compile.
//  - depthClamp and other Vulkan-1.2 fields are absent from the 1.1 headers this tree builds against.
#ifndef GENERALSX_VULKANFNS_H
#define GENERALSX_VULKANFNS_H

#ifdef _WIN32
#if defined(SAGE_USE_VULKAN_CORE) || defined(GENERALSX_VKSTANDALONE)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <cstdio>

#define VKWP1_LOG(...) do { fprintf(stderr, "[VKWP1] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)

// Seed + instance the entry points are imported against. Both written by VulkanCore::loadLoader()
// and createInstance(), read by imp().
extern PFN_vkGetInstanceProcAddr g_gipa;
extern VkInstance g_impInst;

// The whole entry point surface the Vulkan wedges need, generated from one macro list so the struct,
// the import loop and any later table dump cannot drift apart.
#define SAGE_VKFN_LIST \
    VKFN(vkDestroyInstance) VKFN(vkEnumeratePhysicalDevices) VKFN(vkGetPhysicalDeviceProperties) \
    VKFN(vkCreateWin32SurfaceKHR) VKFN(vkDestroySurfaceKHR) \
    VKFN(vkGetPhysicalDeviceSurfaceSupportKHR) VKFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    VKFN(vkGetPhysicalDeviceSurfaceFormatsKHR) VKFN(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    VKFN(vkGetPhysicalDeviceQueueFamilyProperties) VKFN(vkCreateDevice) VKFN(vkDestroyDevice) \
    VKFN(vkGetDeviceQueue) VKFN(vkCreateSwapchainKHR) VKFN(vkGetSwapchainImagesKHR) \
    VKFN(vkAcquireNextImageKHR) VKFN(vkQueuePresentKHR) VKFN(vkDestroySwapchainKHR) \
    VKFN(vkCreateImageView) VKFN(vkDestroyImageView) VKFN(vkCreateFramebuffer) VKFN(vkDestroyFramebuffer) \
    VKFN(vkCreateRenderPass) VKFN(vkDestroyRenderPass) \
    VKFN(vkCreateCommandPool) VKFN(vkDestroyCommandPool) VKFN(vkAllocateCommandBuffers) \
    VKFN(vkBeginCommandBuffer) VKFN(vkEndCommandBuffer) VKFN(vkResetCommandBuffer) \
    VKFN(vkCmdBeginRenderPass) VKFN(vkCmdEndRenderPass) \
    VKFN(vkCreateSemaphore) VKFN(vkDestroySemaphore) VKFN(vkCreateFence) VKFN(vkDestroyFence) \
    VKFN(vkQueueSubmit) VKFN(vkWaitForFences) VKFN(vkResetFences) VKFN(vkDeviceWaitIdle) \
    VKFN(vkGetPhysicalDeviceMemoryProperties) VKFN(vkCreateBuffer) VKFN(vkDestroyBuffer) \
    VKFN(vkGetBufferMemoryRequirements) VKFN(vkAllocateMemory) VKFN(vkFreeMemory) \
    VKFN(vkBindBufferMemory) VKFN(vkMapMemory) VKFN(vkUnmapMemory) \
    VKFN(vkCreateImage) VKFN(vkGetImageMemoryRequirements) VKFN(vkDestroyImage) \
    VKFN(vkBindImageMemory) \
    VKFN(vkCmdCopyBufferToImage) VKFN(vkCmdCopyImageToBuffer) VKFN(vkCmdPipelineBarrier) \
    VKFN(vkCmdClearColorImage) VKFN(vkCmdClearDepthStencilImage) \
    VKFN(vkCreateShaderModule) VKFN(vkDestroyShaderModule) \
    VKFN(vkCreatePipelineLayout) VKFN(vkDestroyPipelineLayout) \
    VKFN(vkCreateGraphicsPipelines) VKFN(vkDestroyPipeline) \
    VKFN(vkCmdBindPipeline) VKFN(vkCmdDraw) VKFN(vkCmdSetViewport) VKFN(vkCmdSetScissor) \
    VKFN(vkCmdBindVertexBuffers) VKFN(vkCmdBindIndexBuffer) VKFN(vkCmdDrawIndexed) \
    VKFN(vkCmdClearAttachments) VKFN(vkCmdBlitImage) \
    VKFN(vkFreeCommandBuffers) \
    VKFN(vkCreateSampler) VKFN(vkDestroySampler) \
    VKFN(vkCreateDescriptorSetLayout) VKFN(vkDestroyDescriptorSetLayout) \
    VKFN(vkCreateDescriptorPool) VKFN(vkDestroyDescriptorPool) VKFN(vkResetDescriptorPool) \
    VKFN(vkAllocateDescriptorSets) VKFN(vkUpdateDescriptorSets) VKFN(vkFreeDescriptorSets) \
    VKFN(vkCmdBindDescriptorSets) \
    VKFN(vkGetPhysicalDeviceFormatProperties) VKFN(vkEnumerateDeviceExtensionProperties) \
    VKFN(vkGetPhysicalDeviceFeatures)

struct Vkfns {
#define VKFN(name) PFN_##name name = nullptr;
    SAGE_VKFN_LIST
#undef VKFN
};
extern Vkfns G;

// Import one entry point through the seed. Passing VK_NULL_HANDLE as the instance asks for the
// global-level commands, which is how vkCreateInstance itself is fetched before an instance exists.
template <typename T>
inline bool imp(const char* name, T& fn)
{
    fn = (T)(void*)g_gipa(g_impInst ? g_impInst : (VkInstance)VK_NULL_HANDLE, name);
    if (!fn) { VKWP1_LOG("FAIL import %s", name); return false; }
    return true;
}

inline bool chk(const char* what, VkResult r)
{
    if (r != VK_SUCCESS) { VKWP1_LOG("FAIL %s VkResult=%d", what, (int)r); return false; }
    return true;
}

// Environment gate reader: returns true only when GENERALSX_<name> starts with '1'. Lives here
// because every Vulkan wedge (5-8) needs the same gate and the gate must stay one implementation.
inline bool vk_env_gate(const char* name)
{
    char buf[16] = { 0 };
    DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
    return n > 0 && buf[0] == '1';
}

// Process-wide submit serialiser (defined in VulkanCore.cpp). Re-entrant on the owning thread, so the
// main thread nesting a mirror inside a frame is fine.
void vk_queue_lock();
void vk_queue_unlock();

#endif // SAGE_USE_VULKAN_CORE || GENERALSX_VKSTANDALONE
#endif // _WIN32
#endif // GENERALSX_VULKANFNS_H
