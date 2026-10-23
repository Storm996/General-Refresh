// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 1: Vulkan device core implementation.
// See Backend/VulkanCore.h for scope. All entry points are loaded dynamically (seed
// vkGetInstanceProcAddr via GetProcAddress on vulkan-1.dll, then import the rest through
// the instance), so the only link inputs are user32/gdi32; the loader is needed at runtime only.
// Behavior is a 1:1 port of the validated standalone skeleton (vkbackend.cpp) from
// "vulkan_wp1_skeleton"; only logging (stderr, release-visible per AGENTS.md) and the
// env-gated singleton hooks were added.
#ifdef _WIN32
#if defined(SAGE_USE_VULKAN_CORE) || defined(GENERALSX_VKSTANDALONE)

#include "VulkanCore.h"
#include "VulkanFns.h" // wedge 5.0: shared entry-point table, logging macro and queue lock
#include "VulkanGeo.h"
#include "VulkanDesc.h"
#include "VulkanDraw.h"  // wedge 5: the draw path, in its own translation unit
#include "VkShadersTri.h" // wedge 5: embedded SPIR-V for the draw-path probe
#include "VkShadersDesc.h" // wedge 4: embedded SPIR-V for the descriptor/constant probe
#include "VkShadersFF.h"   // wedge 6: embedded SPIR-V for the fixed-function vertex stage + its probe FS
#include "VulkanFF.h"      // wedge 6: the CPU reference the probe compares against
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>

// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.3: serialize shared-queue use.
// The mirror phase runs Vulkan work from three threads: the per-frame present chain (main), the
// threaded texture loader calling VKTex_Upload_2D (worker), and now the CPU-dynamic writers
// (terrain/water/tree, main but interleaved with the loader). vkQueueSubmit / the shared command
// pool / fence waits are NOT safe to drive concurrently from one queue, so every submit-and-wait
// sequence takes this one process-wide critical section. Re-entrant on the owning thread, so the
// main thread nesting a mirror inside a frame is fine. Full async upload queues replace this at
// wedge 5 (descriptors + per-thread command buffers).
// GeneralsX @refactor Qoder 18/10/2026 wedge 5.0: the lock moved out of file-static scope - wedges
// 5-8 split into their own translation units and all of them submit through the same queue, so they
// must take the same section. Declared in VulkanFns.h.
static CRITICAL_SECTION g_vkQueueCS;
static volatile long    g_vkQueueCSState = 0; // 0 uninit, 1 initing, 2 ready
static void vk_queue_cs_init()
{
    long s = InterlockedCompareExchange(&g_vkQueueCSState, 1, 0);
    if (s == 0) { InitializeCriticalSection(&g_vkQueueCS); InterlockedExchange(&g_vkQueueCSState, 2); }
    else { while (g_vkQueueCSState != 2) Sleep(0); }
}
void vk_queue_lock()   { vk_queue_cs_init(); EnterCriticalSection(&g_vkQueueCS); }
void vk_queue_unlock() { LeaveCriticalSection(&g_vkQueueCS); }

// The single instances of the shared loader surface (declared by VulkanFns.h).
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
VkInstance g_impInst = VK_NULL_HANDLE;
Vkfns G = {};

// forward: texture-mirror teardown (defined with the wedge 3.2 helpers at the bottom, global scope)
void vktex_shutdown(VkDevice dev);
void vkrt_shutdown(VkDevice dev); // wedge 3.4 render-target/depth mirror teardown

// GeneralsX @feature Qoder 20/10/2026 standalone exe defaults: the pure-Vulkan backend used to need an
// eleven-line launcher script (VKCORE/VKPRESENT/VKGEO/VKTEX/VKRT/VKDESC/VKDRAW/VKFFVERT/VKFFPIX/VKPGM
// and VK_NOD3D). Those gates are still the contract, but a shipping exe should not depend on a script:
// this constructor runs before WinMain and fills in any gate the user did not set explicitly. An
// explicit value (including 0) always wins, so every QA A/B leg keeps working unchanged.
//
// GENERALSX_VK_PURE=0 opts the whole defaulting step out, which is the one switch a developer needs to
// drive the build back to its env-driven mode without touching eleven variables.
static void vk_install_pure_runtime_defaults()
{
    char v[8] = {};
    if (GetEnvironmentVariableA("GENERALSX_VK_PURE", v, (DWORD)sizeof(v)) > 0 && v[0] == '0')
        return;
    static const char* const gates[] = {
        "GENERALSX_VKCORE", "GENERALSX_VKPRESENT", "GENERALSX_VKGEO",
        "GENERALSX_VKTEX", "GENERALSX_VKRT", "GENERALSX_VKDESC",
        "GENERALSX_VKDRAW", "GENERALSX_VKFFVERT", "GENERALSX_VKFFPIX",
        "GENERALSX_VKPGM", "GENERALSX_VK_NOD3D",
    };
    unsigned filled = 0;
    for (unsigned i = 0; i < (unsigned)(sizeof(gates) / sizeof(gates[0])); ++i) {
        if (GetEnvironmentVariableA(gates[i], v, (DWORD)sizeof(v)) == 0) {
            // _putenv_s updates both the process environment (read by GetEnvironmentVariableA) and the
            // CRT copy (read by getenv), which the NOD3D checks in dx8vertexbuffer / WinMain use.
            _putenv_s(gates[i], "1");
            ++filled;
        }
    }
    if (filled) {
        fprintf(stderr, "[VKPURE] standalone defaults: filled %u unset Vulkan gate(s); "
            "set GENERALSX_VK_PURE=0 to disable and drive them from the environment\n", filled);
        fflush(stderr);
    }
}

struct VKPureRuntimeDefaults {
    VKPureRuntimeDefaults() { vk_install_pure_runtime_defaults(); }
};
static VKPureRuntimeDefaults g_vkPureRuntimeDefaults;


bool VulkanCore::loadLoader()
{
    m_loader = LoadLibraryA("vulkan-1.dll");
    if (!m_loader) { VKWP1_LOG("FAIL vulkan-1.dll"); return false; }
    g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(m_loader, "vkGetInstanceProcAddr");
    if (!g_gipa) { VKWP1_LOG("FAIL gipa seed"); return false; }
    VKWP1_LOG("loader ok");
    return true;
}

bool VulkanCore::createInstance(const char* appTag)
{
    PFN_vkCreateInstance createInstance;
    if (!imp("vkCreateInstance", createInstance)) return false;
    const char* iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = appTag;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
    if (!chk("vkCreateInstance", createInstance(&ici, nullptr, &m_instance))) return false;
    g_impInst = m_instance;
    VKWP1_LOG("instance ok");
    return true;
}

bool VulkanCore::importAll()
{
#define VKFN(name) if (!imp(#name, G.name)) return false;
    SAGE_VKFN_LIST
#undef VKFN
    VKWP1_LOG("entrypoints ok");
    return true;
}

bool VulkanCore::pickPhysical()
{
    uint32_t n = 0;
    if (!chk("enumeratePhys", G.vkEnumeratePhysicalDevices(m_instance, &n, nullptr)) || !n) return false;
    std::vector<VkPhysicalDevice> pds(n);
    if (!chk("enumeratePhys2", G.vkEnumeratePhysicalDevices(m_instance, &n, pds.data()))) return false;
    for (auto cand : pds) {
        uint32_t qn = 0;
        G.vkGetPhysicalDeviceQueueFamilyProperties(cand, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        G.vkGetPhysicalDeviceQueueFamilyProperties(cand, &qn, qs.data());
        for (uint32_t i = 0; i < qn; ++i) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 ok = VK_FALSE;
            if (!chk("surfaceSupport", G.vkGetPhysicalDeviceSurfaceSupportKHR(cand, i, m_surface, &ok))) return false;
            if (ok) {
                m_phys = cand; m_qfi = i;
                VkPhysicalDeviceProperties props; G.vkGetPhysicalDeviceProperties(cand, &props);
                VKWP1_LOG("phys: %s", props.deviceName);
                return true;
            }
        }
    }
    VKWP1_LOG("FAIL no graphics+present device");
    return false;
}

bool VulkanCore::createDeviceAndQueue()
{
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = m_qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;

    // GeneralsX @feature Qoder 18/10/2026 wedge 4: the sampler LUT has to be able to hand out
    // anisotropic samplers, because D3DSAMP_MAXANISOTROPY>1 is a legal engine setting and the fixed
    // function path maps D3DTEXF_ANISOTROPIC onto it. D3DTADDRESS_MIRRORONCE needs the
    // mirror-clamp-to-edge address mode, which is a device extension on a 1.1 device (core in 1.2),
    // so ask for it only when this driver advertises it. Enabling an unsupported feature or extension
    // would fail vkCreateDevice outright, hence the pre-query. The same call site records
    // minUniformBufferOffsetAlignment / minStorageBufferOffsetAlignment, which the constant rings must
    // respect for the offsets handed to vkCmdBindDescriptorSets.
    std::vector<const char*> dext;
    dext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    uint32_t dn = 0;
    if (G.vkEnumerateDeviceExtensionProperties(m_phys, nullptr, &dn, nullptr) == VK_SUCCESS && dn) {
        std::vector<VkExtensionProperties> dp(dn);
        if (G.vkEnumerateDeviceExtensionProperties(m_phys, nullptr, &dn, dp.data()) == VK_SUCCESS) {
            for (auto& e : dp) {
                if (!strcmp(e.extensionName, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) {
                    dext.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
                    m_mirrorClampEdge = true;
                }
            }
        }
    }
    VkPhysicalDeviceFeatures feats = {};
    VkPhysicalDeviceProperties props = {};
    VkPhysicalDeviceLimits limits = {};
    G.vkGetPhysicalDeviceFeatures(m_phys, &feats);
    // There is no vkGetPhysicalDeviceLimits entry point: the limits ride inside
    // VkPhysicalDeviceProperties, and this call was already imported for the device name.
    G.vkGetPhysicalDeviceProperties(m_phys, &props);
    limits = props.limits;
    m_anisotropic = feats.samplerAnisotropy != 0;
    m_maxAnisotropy = limits.maxSamplerAnisotropy;
    // feats now holds exactly the feature bits this device supports, so passing it through enables
    // anisotropy and non-solid fill (D3DFILL_WIREFRAME, which wedge 0 saw in the census) where they
    // exist and leaves them off where they do not. Never force a bit here: requesting an unsupported
    // feature makes vkCreateDevice fail.
    dci.enabledExtensionCount = (uint32_t)dext.size();
    dci.ppEnabledExtensionNames = dext.data();
    dci.pEnabledFeatures = &feats;

    if (!chk("CreateDevice", G.vkCreateDevice(m_phys, &dci, nullptr, &m_device))) return false;
    G.vkGetDeviceQueue(m_device, m_qfi, 0, &m_queue);
    m_uniformAlign = limits.minUniformBufferOffsetAlignment;
    m_storageAlign = limits.minStorageBufferOffsetAlignment;
    if (m_uniformAlign == 0 || (m_uniformAlign & (m_uniformAlign - 1)) != 0) m_uniformAlign = 256;
    if (m_storageAlign == 0 || (m_storageAlign & (m_storageAlign - 1)) != 0) m_storageAlign = 32;
    VKWP1_LOG("device+queue ok  uboAlign=%llu ssboAlign=%llu aniso=%d mirrorClamp=%d maxAniso=%.1f",
        (unsigned long long)m_uniformAlign, (unsigned long long)m_storageAlign, (int)m_anisotropic,
        (int)m_mirrorClampEdge, (double)m_maxAnisotropy);
    return true;
}

bool VulkanCore::createRenderPass()
{
    VkAttachmentDescription att = {};
    att.format = m_format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1; rpi.pDependencies = &dep;
    if (!chk("CreateRenderPass", G.vkCreateRenderPass(m_device, &rpi, nullptr, &m_renderPass))) return false;
    VKWP1_LOG("renderpass ok");
    return true;
}

bool VulkanCore::createSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    if (!chk("caps", G.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_phys, m_surface, &caps))) return false;
    uint32_t fmn = 0;
    if (!chk("formats", G.vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, m_surface, &fmn, nullptr)) || !fmn) return false;
    std::vector<VkSurfaceFormatKHR> fmts(fmn);
    if (!chk("formats2", G.vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, m_surface, &fmn, fmts.data()))) return false;
    VkSurfaceFormatKHR sf = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) sf = f;

    // currentExtent is (0xFFFFFFFF,...) only for pretransform/vsync-ish oddities; clamp to window.
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        RECT rc; GetClientRect(m_hwnd, &rc);
        ext.width  = (uint32_t)(rc.right - rc.left);
        ext.height = (uint32_t)(rc.bottom - rc.top);
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    }

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) want = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sw = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sw.surface = m_surface; sw.minImageCount = want;
    sw.imageFormat = sf.format; sw.imageColorSpace = sf.colorSpace;
    sw.imageExtent = ext; sw.imageArrayLayers = 1;
    sw.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                  | VK_IMAGE_USAGE_SAMPLED_BIT       // wedge 4: the present chain samples the scene mirror
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT  //        so it can also be probed by a copy
                  | VK_IMAGE_USAGE_TRANSFER_DST_BIT; // wedge 5: the scene mirror blits into the back buffer
    sw.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sw.preTransform = caps.currentTransform;
    sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // GeneralsX @performance Qoder 19/10/2026 R-05: retail default stays FIFO (tear-free). A benchmarking
    // probe env (GENERALSX_VKPRESENT_NOVSYNC) lets a QA run pick MAILBOX then IMMEDIATE so the measured
    // frame cost is the real CPU+GPU cost rather than the refresh ceiling - the 60Hz floor was hiding how
    // much headroom the layer actually has. Never the default, never a gameplay change.
    sw.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (getenv("GENERALSX_VKPRESENT_NOVSYNC")) {
        uint32_t pmc = 0;
        G.vkGetPhysicalDeviceSurfacePresentModesKHR(m_phys, m_surface, &pmc, nullptr);
        if (pmc) {
            std::vector<VkPresentModeKHR> pms(pmc);
            if (G.vkGetPhysicalDeviceSurfacePresentModesKHR(m_phys, m_surface, &pmc, pms.data())) {
                for (size_t i = 0; i < pms.size(); ++i)
                    if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) { sw.presentMode = pms[i]; break; }
                if (sw.presentMode == VK_PRESENT_MODE_FIFO_KHR)
                    for (size_t i = 0; i < pms.size(); ++i)
                        if (pms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) { sw.presentMode = pms[i]; break; }
            }
        }
        VKWP1_LOG("swapchain present mode probe: %u (FIFO=%u MAILBOX=%u IMMEDIATE=%u)",
            (unsigned)sw.presentMode, (unsigned)VK_PRESENT_MODE_FIFO_KHR,
            (unsigned)VK_PRESENT_MODE_MAILBOX_KHR, (unsigned)VK_PRESENT_MODE_IMMEDIATE_KHR);
    }
    sw.clipped = VK_TRUE;
    if (!chk("CreateSwapchain", G.vkCreateSwapchainKHR(m_device, &sw, nullptr, &m_swap))) return false;
    m_format = sf.format; m_extent = ext;

    uint32_t n = 0;
    if (!chk("images", G.vkGetSwapchainImagesKHR(m_device, m_swap, &n, nullptr)) || !n) return false;
    std::vector<VkImage> imgs(n);
    if (!chk("images2", G.vkGetSwapchainImagesKHR(m_device, m_swap, &n, imgs.data()))) return false;
    m_images.swap(imgs);
    m_views.assign(m_images.size(), nullptr);
    for (size_t i = 0; i < m_images.size(); ++i) {
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = m_images[i]; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = m_format;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (!chk("createView", G.vkCreateImageView(m_device, &vci, nullptr, &m_views[i]))) return false;
    }
    m_fbs.assign(m_views.size(), nullptr);
    for (size_t i = 0; i < m_views.size(); ++i) {
        VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbi.renderPass = m_renderPass;
        fbi.attachmentCount = 1; fbi.pAttachments = &m_views[i];
        fbi.width = m_extent.width; fbi.height = m_extent.height; fbi.layers = 1;
        if (!chk("CreateFramebuffer", G.vkCreateFramebuffer(m_device, &fbi, nullptr, &m_fbs[i]))) return false;
    }
    VKWP1_LOG("swapchain ok %zu images %ux%u fmt=%d", m_images.size(), m_extent.width, m_extent.height, (int)m_format);
    return true;
}

void VulkanCore::destroySwapchainChain()
{
    for (auto fb : m_fbs) if (fb) G.vkDestroyFramebuffer(m_device, fb, nullptr);
    m_fbs.clear();
    for (auto v : m_views) if (v) G.vkDestroyImageView(m_device, v, nullptr);
    m_views.clear();
    m_images.clear();
    if (m_swap) { G.vkDestroySwapchainKHR(m_device, m_swap, nullptr); m_swap = nullptr; }
}

bool VulkanCore::Init(HWND hwnd, const char* appTag)
{
    m_hwnd = hwnd; m_hinst = (HINSTANCE)GetWindowLongPtrA(hwnd, GWLP_HINSTANCE);
    if (!loadLoader()) return false;
    if (!createInstance(appTag)) return false;
    if (!importAll()) return false;
    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = m_hinst; sci.hwnd = m_hwnd;
    if (!chk("CreateWin32Surface", G.vkCreateWin32SurfaceKHR(m_instance, &sci, nullptr, &m_surface))) return false;
    if (!pickPhysical()) return false;
    if (!createDeviceAndQueue()) return false;
    // renderpass wants m_format, so peek the preferred swapchain format first (query, no chain yet).
    {
        uint32_t fmn = 0;
        if (!chk("formats", G.vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, m_surface, &fmn, nullptr)) || !fmn) return false;
        std::vector<VkSurfaceFormatKHR> fmts(fmn);
        if (!chk("formats2", G.vkGetPhysicalDeviceSurfaceFormatsKHR(m_phys, m_surface, &fmn, fmts.data()))) return false;
        VkSurfaceFormatKHR sf = fmts[0];
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) sf = f;
        m_format = sf.format;
    }
    if (!createRenderPass()) return false;
    if (!createSwapchain()) return false;

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = m_qfi;
    if (!chk("CommandPool", G.vkCreateCommandPool(m_device, &cpi, nullptr, &m_pool))) return false;
    m_cmds.assign(3, nullptr);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = m_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 3;
    if (!chk("AllocCmds", G.vkAllocateCommandBuffers(m_device, &cai, m_cmds.data()))) return false;
    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < 3; ++i) {
        if (!chk("SemImg", G.vkCreateSemaphore(m_device, &sem, nullptr, &m_imgAvail[i]))) return false;
        if (!chk("SemDone", G.vkCreateSemaphore(m_device, &sem, nullptr, &m_done[i]))) return false;
        if (!chk("Fence",  G.vkCreateFence(m_device, &fi, nullptr, &m_fence[i]))) return false;
    }
    m_firstResizeDone = true;
    // GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: optional geometry ring, env-gated.
    // Ring failure is non-fatal by design: locks then keep the pure D3D9 path.
    {
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKGEO", env, (DWORD)sizeof(env));
        if (n != 0 && env[0] != '0') {
            if (!CreateGeometryRing()) VKWP1_LOG("WARN geo ring unavailable - VB/IB stay on D3D9 locks");
        }
    }
    // GeneralsX @feature Qoder 18/10/2026 WP1 wedge 4: descriptor set layout, pool, sampler LUT and the
    // constant rings. Same non-fatal contract as the ring: without it the mirror phase is unaffected,
    // with it the wedge-3 resources become bindable.
    if (!VKDesc_Create()) {
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKDESC", env, (DWORD)sizeof(env));
        if (n != 0 && env[0] != '0') VKWP1_LOG("WARN descriptor layer unavailable - wedges 5+ cannot bind");
    }
    VKWP1_LOG("Init ok");
    return true;
}

bool VulkanCore::Resize()
{
    if (!m_device) return false;
    G.vkDeviceWaitIdle(m_device);
    destroySwapchainChain();
    if (!createSwapchain()) return false;
    VKWP1_LOG("Resize ok");
    return true;
}

uint32_t g_lastAcquiredImage = 0; // per-frame scratch; kept file-static to avoid bloating the header

// ---- wedge 3.1: host-visible geometry ring --------------------------------------
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: one 64 MiB host-visible coherent buffer,
// persistently mapped; DX8 VB/IB lock classes mirror writes here via VKGeo_Alloc (see VulkanGeo.h).
bool VulkanCore::CreateGeometryRing()
{
    const VkDeviceSize cap = 64ull << 20;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = cap;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
              | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("CreateBuffer(geo)", G.vkCreateBuffer(m_device, &bci, nullptr, &m_geoBuffer))) {
        m_geoBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    G.vkGetBufferMemoryRequirements(m_device, m_geoBuffer, &req);
    VkPhysicalDeviceMemoryProperties mp;
    G.vkGetPhysicalDeviceMemoryProperties(m_phys, &mp);
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t mt = 0;
    bool found = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) { mt = i; found = true; break; }
    }
    if (!found) {
        VKWP1_LOG("FAIL geo ring: no host-visible+coherent memory type");
        G.vkDestroyBuffer(m_device, m_geoBuffer, nullptr);
        m_geoBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    if (!chk("AllocateMemory(geo)", G.vkAllocateMemory(m_device, &mai, nullptr, &m_geoMemory))) {
        G.vkDestroyBuffer(m_device, m_geoBuffer, nullptr);
        m_geoBuffer = VK_NULL_HANDLE; m_geoMemory = VK_NULL_HANDLE;
        return false;
    }
    if (!chk("BindBufferMemory(geo)", G.vkBindBufferMemory(m_device, m_geoBuffer, m_geoMemory, 0))) {
        G.vkFreeMemory(m_device, m_geoMemory, nullptr);
        G.vkDestroyBuffer(m_device, m_geoBuffer, nullptr);
        m_geoBuffer = VK_NULL_HANDLE; m_geoMemory = VK_NULL_HANDLE;
        return false;
    }
    if (!chk("MapMemory(geo)", G.vkMapMemory(m_device, m_geoMemory, 0, cap, 0, &m_geoMap))) {
        G.vkUnmapMemory(m_device, m_geoMemory);
        G.vkFreeMemory(m_device, m_geoMemory, nullptr);
        G.vkDestroyBuffer(m_device, m_geoBuffer, nullptr);
        m_geoBuffer = VK_NULL_HANDLE; m_geoMemory = VK_NULL_HANDLE; m_geoMap = nullptr;
        return false;
    }
    m_geoCap = cap;
    m_geoCur = 0; m_geoPeak = 0; m_geoAllocs = 0; m_geoRewinds = 0;
    VKWP1_LOG("geo ring ok: 64 MiB host-visible mapped at %p", m_geoMap);
    return true;
}

void* VulkanCore::Geo_Alloc(unsigned int size, bool discard)
{
    if (!GeoRingUp() || size == 0) return nullptr;
    unsigned long long sz = size;
    if (sz > m_geoCap) return nullptr;
    unsigned long long off = discard ? 0ull : ((m_geoCur + 255ull) & ~255ull);
    if (off + sz > m_geoCap) { off = 0; ++m_geoRewinds; }  // ring full: reuse from start (mirror-only phase)
    m_geoCur = off + sz;
    if (m_geoCur > m_geoPeak) m_geoPeak = m_geoCur;
    ++m_geoAllocs;
    if ((m_geoAllocs & 8191ull) == 0ull) {
        VKWP1_LOG("geo ring: allocs=%llu rewinds=%llu peak=%llu KiB cur=%llu KiB",
            m_geoAllocs, m_geoRewinds, m_geoPeak >> 10, m_geoCur >> 10);
    }
    return (char*)m_geoMap + off;
}

bool VulkanCore::BeginFrame(float r, float g, float b)
{
    uint32_t slot = m_frame % 3;
    if (!chk("waitFence", G.vkWaitForFences(m_device, 1, &m_fence[slot], VK_TRUE, ~0ull))) return false;
    if (!chk("resetFence", G.vkResetFences(m_device, 1, &m_fence[slot]))) return false;
    uint32_t img = 0;
    VkResult ar = G.vkAcquireNextImageKHR(m_device, m_swap, ~0ull, m_imgAvail[slot], nullptr, &img);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) { if (!Resize()) return false;
        ar = G.vkAcquireNextImageKHR(m_device, m_swap, ~0ull, m_imgAvail[slot], nullptr, &img); }
    if (!chk("acquire", ar)) return false;
    g_lastAcquiredImage = img;

    if (!chk("resetCmd", G.vkResetCommandBuffer(m_cmds[slot], 0))) return false;
    VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!chk("beginCmd", G.vkBeginCommandBuffer(m_cmds[slot], &bb))) return false;
    VkClearValue clear = {};
    clear.color.float32[0] = r; clear.color.float32[1] = g; clear.color.float32[2] = b; clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = m_renderPass; rbi.framebuffer = m_fbs[img];
    rbi.renderArea = { {0,0}, m_extent };
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    G.vkCmdBeginRenderPass(m_cmds[slot], &rbi, VK_SUBPASS_CONTENTS_INLINE);
    G.vkCmdEndRenderPass(m_cmds[slot]);
    if (!chk("endCmd", G.vkEndCommandBuffer(m_cmds[slot]))) return false;
    return true;
}

bool VulkanCore::EndFrame()
{
    uint32_t slot = m_frame % 3;
    VkPipelineStageFlags waitAt = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &m_imgAvail[slot];
    si.pWaitDstStageMask = &waitAt;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &m_done[slot];
    si.commandBufferCount = 1; si.pCommandBuffers = &m_cmds[slot];
    VkResult pr;
    vk_queue_lock();
    if (!chk("submit", G.vkQueueSubmit(m_queue, 1, &si, m_fence[slot]))) { vk_queue_unlock(); return false; }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &m_done[slot];
    pi.swapchainCount = 1; pi.pSwapchains = &m_swap; pi.pImageIndices = &g_lastAcquiredImage;
    pr = G.vkQueuePresentKHR(m_queue, &pi);
    Advance_Frame();
    vk_queue_unlock();
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) return Resize();
    return chk("present", pr);
}

// One frame boundary. Every queue that ages with the in-flight window is serviced here, so the wedge-2
// clear path and the wedge-5 draw path retire resources at the same rate.
unsigned long long g_vkFrameNo = 0;
unsigned long long VKCore_Frame_No() { return g_vkFrameNo; }
void VulkanCore::Advance_Frame()
{
    ++m_frame;
    ++g_vkFrameNo;
    Reap_Mirrors();
}

// Hand an image this layer no longer binds to the reaper. See VulkanCore.h: the caller has already
// removed it from its own table, so nothing can name it again from here on - but a frame submitted
// before that moment may still be running.
void VulkanCore::Retire_Mirror(VkImage image, VkImageView view, VkDeviceMemory mem)
{
    if (!image && !view && !mem) return;
    RetiredMirror r;
    r.image = image; r.view = view; r.mem = mem;
    r.framesLeft = VK_IN_FLIGHT + 1;
    m_retired.push_back(r);
    ++m_mirRetired;
}

void VulkanCore::Reap_Mirrors()
{
    if (m_retired.empty() || !m_device) return;
    for (size_t i = 0; i < m_retired.size(); ) {
        if (m_retired[i].framesLeft > 1u) { --m_retired[i].framesLeft; ++i; continue; }
        if (m_retired[i].view) G.vkDestroyImageView(m_device, m_retired[i].view, nullptr);
        if (m_retired[i].image) G.vkDestroyImage(m_device, m_retired[i].image, nullptr);
        if (m_retired[i].mem) G.vkFreeMemory(m_device, m_retired[i].mem, nullptr);
        m_retired.erase(m_retired.begin() + (long)i);
        ++m_mirReaped;
    }
}

void VulkanCore::Shutdown()
{
    if (m_device) G.vkDeviceWaitIdle(m_device);
    // The idle above retired every submission that could name a queued mirror, so drain the queue now
    // rather than leak it.
    while (!m_retired.empty()) {
        const RetiredMirror& r = m_retired.back();
        if (m_device) {
            if (r.view) G.vkDestroyImageView(m_device, r.view, nullptr);
            if (r.image) G.vkDestroyImage(m_device, r.image, nullptr);
            if (r.mem) G.vkFreeMemory(m_device, r.mem, nullptr);
        }
        m_retired.pop_back();
    }
    VKDesc_Destroy();   // wedge 4: samplers, descriptor pool, constant rings, white fallback
    vktex_shutdown(m_device);
    vkrt_shutdown(m_device);
    for (int i = 0; i < 3; ++i) {
        if (m_done[i]) G.vkDestroySemaphore(m_device, m_done[i], nullptr);
        if (m_imgAvail[i]) G.vkDestroySemaphore(m_device, m_imgAvail[i], nullptr);
        if (m_fence[i]) G.vkDestroyFence(m_device, m_fence[i], nullptr);
    }
    if (m_pool) G.vkDestroyCommandPool(m_device, m_pool, nullptr);
    // wedge 3.1: geometry ring first (device still alive for unmap/free)
    if (m_geoMap) { G.vkUnmapMemory(m_device, m_geoMemory); m_geoMap = nullptr; }
    if (m_geoMemory) { G.vkFreeMemory(m_device, m_geoMemory, nullptr); m_geoMemory = VK_NULL_HANDLE; }
    if (m_geoBuffer) { G.vkDestroyBuffer(m_device, m_geoBuffer, nullptr); m_geoBuffer = VK_NULL_HANDLE; }
    destroySwapchainChain();
    if (m_renderPass) G.vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    if (m_device) G.vkDestroyDevice(m_device, nullptr);
    if (m_surface) G.vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_instance) G.vkDestroyInstance(m_instance, nullptr);
    m_device = nullptr; m_renderPass = nullptr; m_pool = nullptr; m_surface = nullptr; m_instance = nullptr;
    if (m_loader) { FreeLibrary(m_loader); m_loader = nullptr; }
    g_gipa = nullptr; g_impInst = VK_NULL_HANDLE;
    VKWP1_LOG("Shutdown ok");
}

// ---- facade hooks ----------------------------------------------------------
namespace {
VulkanCore* g_vulkanCore = nullptr; // single co-created core for wedge 1
}

bool VulkanCore_Try_Init(HWND hwnd)
{
    char env[8] = {};
    DWORD n = GetEnvironmentVariableA("GENERALSX_VKCORE", env, (DWORD)sizeof(env));
    if (n == 0 || env[0] == '0') {
        VKWP1_LOG("wedge idle (set GENERALSX_VKCORE=1 to co-create)");
        return false;
    }
    if (g_vulkanCore) return true; // already up
    g_vulkanCore = new VulkanCore();
    if (!g_vulkanCore->Init(hwnd, "GeneralsX ZH")) {
        VKWP1_LOG("FAIL co-create on HWND %p - continuing D3D9-only", (void*)hwnd);
        delete g_vulkanCore;
        g_vulkanCore = nullptr;
        return false;
    }
    VKWP1_LOG("co-created on engine HWND %p; presentation still D3D9", (void*)hwnd);
    // wedge 3.4: prove the render-target/depth layout state machine + readback primitive against this
    // GPU once, at co-create time (no engine RT exists yet). No-op unless GENERALSX_VKRT=1.
    VKRT_Run_Probe();
    // wedge 4: prove the descriptor/constant/sampler layer by drawing into a keyed RT mirror.
    // No-op unless GENERALSX_VKDESC=1.
    VKDesc_Run_Probe();
#if defined(SAGE_USE_VULKAN_CORE)
    // GeneralsX @feature Qoder 18/10/2026 wedge 5: stand up the draw path. It renders into the wedge 3.4
    // mirrors with the wedge 4 set layout, so it is created after both probes and refuses itself (with a
    // log naming the missing gate) when either is off. No-op unless GENERALSX_VKDRAW=1.
    if (VKDraw_Create()) VKWP1_LOG("wedge5 draw path created");
    // wedge 6: the fixed-function vertex stage, measured against the CPU reference in VulkanFF.cpp.
    // No-op unless GENERALSX_VKFFVERT=1.
    VKFF_Run_Probe();
    // wedge 7: the fixed-function pixel stage - one interpreter for every combiner, measured against
    // VKFF_Combine_Ref band by band. No-op unless GENERALSX_VKFFPIX=1.
    VKFF_Run_Pixel_Probe();
#endif
    return true;
}

void VulkanCore_Shutdown()
{
    if (!g_vulkanCore) return;
#if defined(SAGE_USE_VULKAN_CORE)
    // Wedge 5 owns pipelines, passes and geometry buffers on this device, so it tears down first.
    VKDraw_Destroy();
#endif
    g_vulkanCore->Shutdown();
    delete g_vulkanCore;
    g_vulkanCore = nullptr;
}

bool VulkanCore_Present_Enabled()
{
    if (!g_vulkanCore) return false;
    char env[8] = {};
    DWORD n = GetEnvironmentVariableA("GENERALSX_VKPRESENT", env, (DWORD)sizeof(env));
    return n != 0 && env[0] != '0';
}

void VulkanCore_Present_Frame()
{
    static bool s_logged = false;
    if (!g_vulkanCore) return;
#if defined(SAGE_USE_VULKAN_CORE)
    // GeneralsX @feature Qoder 18/10/2026 wedge 5: once the draw path is up, the frame it recorded is what
    // gets presented - the scene mirror is blitted into the swapchain image instead of a solid clear.
    if (VKDraw_Ready()) {
        if (!s_logged) { VKWP1_LOG("wedge5: presenting the recorded engine frame (scene mirror -> swapchain)"); s_logged = true; }
        VKDraw_End_Frame();
        VKDraw_Present();
        if (VKDraw_Resize_Needed()) {
            VKWP1_LOG("wedge5: swapchain out of date - recreating");
            g_vulkanCore->Resize();
        }
        return;
    }
#endif
    if (!s_logged) { VKWP1_LOG("wedge2: Vulkan swapchain now owns display (clear = generals purple)"); s_logged = true; }
    if (!g_vulkanCore->BeginFrame(0.25f, 0.05f, 0.60f)) { VKWP1_LOG("present BeginFrame failed"); return; }
    (void)g_vulkanCore->EndFrame();
}

void VulkanCore_Resize()
{
    if (g_vulkanCore) g_vulkanCore->Resize();
}

// ---- wedge 3.1 bridge hooks (declared in VulkanGeo.h) ---------------------------
bool VKGeo_Ready(unsigned size)
{
    if (!g_vulkanCore || !g_vulkanCore->GeoRingUp()) return false;
    return size > 0 && (unsigned long long)size <= 64ull << 20;
}

void* VKGeo_Alloc(unsigned size, bool discard)
{
    if (!g_vulkanCore) return nullptr;
    return g_vulkanCore->Geo_Alloc(size, discard);
}

// ---- mirror identity (R-07): the recycled-pointer guard both tables share ------------------
// GeneralsX @bugfix Qoder 20/10/2026 R-07: both mirror tables are keyed by a raw D3D object pointer and
// the allocator recycles those addresses. The shape guard catches the reuse only when width, height,
// level count or format differ; a recycled 256x256 DXT1 landing on a dead 256x256 DXT1 was invisible and
// the new owner sampled the dead texture's texels. The backend that owns the objects answers with a
// per-object token, stored on the mirror at create and required at every lookup, so a reuse can neither
// inherit an image nor keep one after its owner dies. 0 means "nobody can name this object", which is the
// state of the d3d9/DXVK path and of the keys this layer owns itself (the scene mirror, the probes).
VKMirror_Identify g_vkIdentify = nullptr;

void VK_Set_Mirror_Identity_Fn(VKMirror_Identify fn) { g_vkIdentify = fn; }

static unsigned long long vk_mirror_identity(void* key)
{
    if (!g_vkIdentify || !key) return 0ull;
    return g_vkIdentify(key);
}

// The census a forensic run reads: every time a mirror is found to belong to a dead object, one line names
// the table, the pointer and both tokens. Budgeted, because the load-time burst (hundreds of recycled
// texture addresses in the first frames) would otherwise fill the log.
static unsigned long long g_vkIdentDrops = 0ull;
static void vk_log_ident_drop(const char* table, void* key, unsigned long long built, unsigned long long live)
{
    ++g_vkIdentDrops;
    if (g_vkIdentDrops <= 64ull) {
        VKWP1_LOG("mirror identity drop: %s key=%p built=%llu live=%llu total=%llu",
            table, key, built, live, g_vkIdentDrops);
    }
}

// ---- wedge 3.2: static texture mirror ----------------------------------------------
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.2: every fully-loaded MANAGED texture is
// mirrored into its own device-local VkImage through a growable host-visible staging buffer.
// One synchronous one-shot submit per texture (fence-waited) keeps the mirror phase trivially
// correct; descriptor wiring and eviction replace this at wedge 5. Env gate GENERALSX_VKTEX.
namespace {
struct VKTexFmtInfo {
    VkFormat vkf;
    unsigned bytes;
    bool block;
    bool exact;
    bool rot4;
    // GeneralsX @bugfix Qoder 18/10/2026 D3D and Vulkan disagree about what some formats' stored
    // bits mean (X1R5G5B5's X bit is alpha=1 in D3D, A8 is (0,0,0,A), R8G8B8 is BGR-in-memory).
    // Until now every view was identity-swizzled, so those channels reached the shader rotated or
    // wrong. The mapping rides on the format row and is applied where the view is created; the
    // stored bytes stay byte-identical, so the exact readback hash remains valid.
    // Zero-initialised members are VK_COMPONENT_SWIZZLE_IDENTITY, so an omitted field is identity.
    VkComponentMapping map;
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: VK_FORMAT_R8G8B8_UNORM - the honest
    // Vulkan match for D3D's R8G8B8 - is an optional format, and the NVIDIA driver does not merely refuse
    // it: vkCreateImage faults with an integer divide (the no-d3d9 radar mirror died there at 0xC0000094).
    // Those textures mirror into the mandatory B8G8R8A8 instead, which is four bytes per texel rather than
    // the three the surface stores, so the upload expands each pixel and the readback hash no longer
    // applies (exact is false wherever this is set).
    bool expand3;
};
// A mirror slot is addressed by the D3D texture pointer. DXVK recycles freed COM addresses, so the
// pointer alone does NOT identify the content: a later, differently-shaped texture can land on a stale
// address. We therefore also record the shape (dims/levels/format) that built the image and only reuse
// it when every field matches - a genuine in-place CPU update (terrain atlas, water bump) matches and
// re-uploads, while a recycled-pointer mismatch destroys the old image and creates a fresh one instead
// of recording an illegal copy into it (which is what device-lost the first mirror submit here).
// GeneralsX @bugfix Qoder 21/10/2026 multi-match exhaustion: 'size' is the image's allocation estimate.
// TX.vramBytes is the LIVE total (create adds, every drop/replace subtracts); without the per-entry size
// the budget could only ever grow and, once over 3 GiB, refused every later mirror forever - the fonts
// went white first and the next map lost every texture.
struct VKTexImg { void* key; VkImage img; VkDeviceMemory mem; unsigned w, h, levels; VkFormat vkf; VkImageView view; unsigned long long hash; unsigned reups; unsigned long long ident; unsigned long long size; };

// GeneralsX @performance Qoder 19/10/2026 R-05 wave 9: async mirror upload ring. The R8 probe put
// 4-16 ms of every glyph-atlas page turn inside _Copy_DX8_Rects, and the only ms-scale item that
// path holds is VKTex_Mirror_D3D_Texture's real upload: a shared-queue submit followed by a
// synchronous vkWaitForFences, so the render thread drained the whole GPU queue (4K frame work
// included) on every page. DXVK never paid that - its texture writes are ring-backfilled. Uploads
// now get their OWN staging slot from a ring; the submit is fire-and-forget on the same queue
// (queue order already makes any later draw see the transfer), and the slot is recycled once its
// fence signals. TX.stage stays the synchronous path for readbacks/probe/white-fill, which must
// wait anyway. Only verify (one upload in 512) still waits inline; a ring-full stall is counted.
struct VKTexUpSlot {
    VkBuffer buf = nullptr;
    VkDeviceMemory mem = nullptr;
    void* map = nullptr;
    unsigned long long cap = 0;
    bool busy = false;
};
struct VKTexUpPending { VkFence fence; VkCommandBuffer cb; unsigned slot; };

struct VKTexStateT {
    VkBuffer         stage = nullptr;
    VkDeviceMemory   stageMem = nullptr;
    void*            stageMap = nullptr;
    unsigned long long stageCap = 0;
    std::vector<VKTexImg> imgs;
    std::vector<VKTexUpSlot> upSlots;
    std::vector<VKTexUpPending> upPending;
    unsigned long long uploads = 0, reuploads = 0, hashSkips = 0, vramBytes = 0, fmtSkips = 0, budgetSkips = 0, verifyOK = 0, verifyBad = 0;
    unsigned long long upStalls = 0;
    long long upUs = 0, upMaxUs = 0, upCount = 0;
    bool envOn = false, envChecked = false, budgetWarned = false;
};
VKTexStateT TX;

bool vktex_env_on()
{
    if (!TX.envChecked) {
        TX.envChecked = true;
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKTEX", env, (DWORD)sizeof(env));
        TX.envOn = (n != 0 && env[0] != '0');
    }
    return TX.envOn;
}

// WW3DFormat switch keys the enum ordinals in ww3dformat.h (see that file for the list).
// exact = Vulkan bit layout is byte-identical to the D3D surface, so the readback hash check applies.
bool vktex_fmt(int ww3d, VKTexFmtInfo& o)
{
    o = { VK_FORMAT_UNDEFINED, 0, false, false, false };
    switch (ww3d) {
    // D3D stores R8G8B8 as B,G,R in memory, which is B8G8R8A8's first three bytes; expand3 adds the
    // opaque alpha the surface does not carry. Identity swizzle is then exactly D3D's sampling.
    case 1:  o = { VK_FORMAT_B8G8R8A8_UNORM, 3, false, false, false,
                    { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY }, true }; break;
    case 2:  o = { VK_FORMAT_B8G8R8A8_UNORM, 4, false, true }; break;         // A8R8G8B8 LE storage
    // GeneralsX @bugfix Qoder 19/10/2026 BUG-02: X8R8G8B8's top byte is ignored by D3D (sampling
    // returns alpha 1.0), but the mirror stored it as a fourth channel of B8G8R8A8 and sampled the
    // real byte - which is 0 for every asset the loader filled through a path that does not write it.
    // SRCALPHA-blended UI batches (menu panels, the title-screen frame) then multiplied out to zero
    // and vanished, leaving the black clear to show through around glyphs and logo cut-outs. Same
    // force-A=ONE view as X1R5G5B5/X4R4G4B4/X8L8V8U8 above; stored bytes are untouched.
    case 3:  o = { VK_FORMAT_B8G8R8A8_UNORM, 4, false, true, false,
                    { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE } }; break;
    case 4:  o = { VK_FORMAT_R5G6B5_UNORM_PACK16, 2, false, true }; break;
    // X1R5G5B5's top bit is ignored by D3D (alpha samples as 1.0); Vulkan would sample it as a real
    // 1-bit alpha, so the view forces A=ONE. A1R5G5B5 keeps its stored alpha.
    case 5:  o = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, false, true, false,
                    { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE } }; break;
    case 6:  o = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, false, true }; break;
    // GeneralsX @bugfix Qoder 18/10/2026 A4R4G4B4 stores alpha in bits 12..15 (D3D lists channels
    // MSB first), while the mandatory Vulkan R4G4B4A4 packs red there. Raw-copying the words put the
    // glyph coverage in the red channel and an opaque B nibble in alpha, so font quads came out as flat
    // blocks, and every 64px terrain tile sampled R/G/B/A rotated (alpha read the blue nibble). rot4
    // rotates each word four bits at upload so both formats keep their D3D channel positions.
    case 7:  o = { VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, false, false, true }; break; // A4R4G4B4 nibble-rotated
    // X4R4G4B4: the X nibble becomes the alpha nibble after rot4; D3D ignores it, so force A=ONE.
    case 11: o = { VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2, false, false, true,
                    { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE } }; break;
    // A8 is a coverage mask: D3D samples (0,0,0,A), Vulkan would broadcast it to (A,A,A,1).
    case 9:  o = { VK_FORMAT_R8_UNORM, 1, false, true, false,
                    { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R } }; break;
    case 14: o = { VK_FORMAT_R8_UNORM, 1, false, true }; break;               // L8 (luminance broadcast is already correct)
    // A8L8 stores [L,A] little-endian; D3D samples (L,L,L,A).
    case 15: o = { VK_FORMAT_R8G8_UNORM, 2, false, true, false,
                    { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G } }; break;
    // A4L4 stores alpha in the high nibble; D3D samples (L,L,L,A).
    case 16: o = { VK_FORMAT_R4G4_UNORM_PACK8, 1, false, true, false,
                    { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R } }; break;
    case 17: o = { VK_FORMAT_R8G8_SNORM, 2, false, true }; break;             // U8V8 bump
    // L6V5U5 has no correct Vulkan storage match; mirroring it produced a rotated colour instead.
    // Skipping it makes the draw layer bind the built-in white, which is the same passthrough the
    // D3D combiners used and is immediately visible instead of silently wrong.
    // (case 18 intentionally falls to default.)
    // X8L8V8U8's fourth byte is written as 0, but D3D ignores it (alpha=1.0).
    case 19: o = { VK_FORMAT_R8G8B8A8_SNORM, 4, false, true, false,
                    { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE } }; break;
    case 20: o = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, true, true }; break;    // DXT1
    case 21: case 22: o = { VK_FORMAT_BC2_UNORM_BLOCK, 16, true, true }; break; // DXT2/DXT3 (premult flag only)
    case 23: case 24: o = { VK_FORMAT_BC3_UNORM_BLOCK, 16, true, true }; break;  // DXT4/DXT5
    case 25: o = { VK_FORMAT_R16_SFLOAT, 2, false, true }; break;             // R16F
    case 26: o = { VK_FORMAT_R16G16_SFLOAT, 4, false, true }; break;          // G16R16F storage
    case 27: o = { VK_FORMAT_R16G16B16A16_SFLOAT, 8, false, true }; break;    // A16B16G16R16F storage
    case 28: o = { VK_FORMAT_R32_SFLOAT, 4, false, true }; break;             // R32F
    case 29: o = { VK_FORMAT_R32G32_SFLOAT, 8, false, true }; break;          // G32R32F storage
    case 30: o = { VK_FORMAT_R32G32B32A32_SFLOAT, 16, false, true }; break;   // A32B32G32R32F storage
    case 31: o = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, false, true }; break;
    default: return false; // R3G3B2/A8R3G3B2/paletted/unsupported: mirror phase skips (counter tracks it)
    }
    return true;
}

unsigned vktex_al16(unsigned x) { return (x + 15u) & ~15u; }

void vktex_dims(unsigned w, unsigned h, unsigned lvl, unsigned& lw, unsigned& lh)
{
    lw = w >> lvl; if (!lw) lw = 1;
    lh = h >> lvl; if (!lh) lh = 1;
}

uint32_t vktex_memtype(const VkMemoryRequirements& req, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    G.vkGetPhysicalDeviceMemoryProperties(g_vulkanCore->Phys(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    if (want != 0) { // fall back to any permitted type (device-less VM or unusual heap layouts)
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if (req.memoryTypeBits & (1u << i)) return i;
    }
    return ~0u;
}

bool vktex_ensure_stage(unsigned long long need)
{
    VkDevice dev = g_vulkanCore->Device();
    if (TX.stage && TX.stageCap >= need) return true;
    unsigned long long cap = 64ull << 20;
    while (cap < need) cap <<= 1;
    if (cap > (1024ull << 20)) return false; // absurd texture: bail (mirrors D3D OOM-skip semantics)
    if (TX.stage) { // always idle here: uploads are fence-synchronous
        G.vkUnmapMemory(dev, TX.stageMem);
        G.vkDestroyBuffer(dev, TX.stage, nullptr);
        G.vkFreeMemory(dev, TX.stageMem, nullptr);
        TX.stage = nullptr; TX.stageMem = nullptr; TX.stageMap = nullptr; TX.stageCap = 0;
    }
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = cap;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("CreateBuffer(staging)", G.vkCreateBuffer(dev, &bci, nullptr, &TX.stage))) { TX.stage = nullptr; return false; }
    VkMemoryRequirements req; G.vkGetBufferMemoryRequirements(dev, TX.stage, &req);
    uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == ~0u) { G.vkDestroyBuffer(dev, TX.stage, nullptr); TX.stage = nullptr; return false; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (!chk("AllocateMemory(staging)", G.vkAllocateMemory(dev, &mai, nullptr, &TX.stageMem))) {
        G.vkDestroyBuffer(dev, TX.stage, nullptr); TX.stage = nullptr; TX.stageMem = nullptr; return false;
    }
    if (!chk("BindBufferMemory(staging)", G.vkBindBufferMemory(dev, TX.stage, TX.stageMem, 0)) ||
        !chk("MapMemory(staging)", G.vkMapMemory(dev, TX.stageMem, 0, cap, 0, &TX.stageMap))) {
        if (TX.stageMap) G.vkUnmapMemory(dev, TX.stageMem);
        G.vkFreeMemory(dev, TX.stageMem, nullptr);
        G.vkDestroyBuffer(dev, TX.stage, nullptr);
        TX.stage = nullptr; TX.stageMem = nullptr; TX.stageMap = nullptr; TX.stageCap = 0;
        return false;
    }
    TX.stageCap = cap;
    VKWP1_LOG("tex staging grown to %llu MiB", cap >> 20);
    return true;
}

// ---- R-05 wave 9: async upload ring (see VKTexUpSlot for the why) -------------
// Caller holds vk_queue_lock. A signaled fence means the transfer has read its staging slot and
// the command buffer is done, so both go back to the pool of reusable things.
static void vktex_reap_up_locked()
{
    VkDevice dev = g_vulkanCore->Device();
    for (size_t i = 0; i < TX.upPending.size(); ) {
        const VKTexUpPending& p = TX.upPending[i];
        if (G.vkGetFenceStatus(dev, p.fence) == VK_SUCCESS) {
            G.vkDestroyFence(dev, p.fence, nullptr);
            G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &p.cb);
            if (p.slot < TX.upSlots.size()) TX.upSlots[p.slot].busy = false;
            TX.upPending.erase(TX.upPending.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

// Hand out a staging slot with at least need bytes. Fresh slots are created up to a bounded ring;
// when the ring is full of in-flight uploads only then stall on the oldest fence (counted).
// Returns the slot index, or -1 on allocation failure.
static int vktex_acquire_up_slot_locked(unsigned long long need)
{
    vktex_reap_up_locked();
    for (size_t i = 0; i < TX.upSlots.size(); ++i) {
        if (!TX.upSlots[i].busy && TX.upSlots[i].cap >= need) return (int)i;
    }
    if (TX.upSlots.size() >= 24 && !TX.upPending.empty()) {
        ++TX.upStalls;
        G.vkWaitForFences(g_vulkanCore->Device(), 1, &TX.upPending[0].fence, VK_TRUE, ~0ull);
        vktex_reap_up_locked();
        for (size_t i = 0; i < TX.upSlots.size(); ++i) {
            if (!TX.upSlots[i].busy && TX.upSlots[i].cap >= need) return (int)i;
        }
    }
    VkDevice dev = g_vulkanCore->Device();
    unsigned long long cap = need < (1ull << 20) ? (1ull << 20) : need;
    VKTexUpSlot s;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = cap;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("CreateBuffer(upslot)", G.vkCreateBuffer(dev, &bci, nullptr, &s.buf))) return -1;
    VkMemoryRequirements req; G.vkGetBufferMemoryRequirements(dev, s.buf, &req);
    uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == ~0u) { G.vkDestroyBuffer(dev, s.buf, nullptr); return -1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (!chk("AllocateMemory(upslot)", G.vkAllocateMemory(dev, &mai, nullptr, &s.mem))) {
        G.vkDestroyBuffer(dev, s.buf, nullptr); return -1;
    }
    if (!chk("BindBufferMemory(upslot)", G.vkBindBufferMemory(dev, s.buf, s.mem, 0)) ||
        !chk("MapMemory(upslot)", G.vkMapMemory(dev, s.mem, 0, cap, 0, &s.map))) {
        if (s.map) G.vkUnmapMemory(dev, s.mem);
        G.vkFreeMemory(dev, s.mem, nullptr);
        G.vkDestroyBuffer(dev, s.buf, nullptr);
        return -1;
    }
    s.cap = cap; s.busy = false;
    TX.upSlots.push_back(s);
    return (int)TX.upSlots.size() - 1;
}

static void vktex_shutdown_up_locked(VkDevice dev)
{
    for (size_t i = 0; i < TX.upPending.size(); ++i) {
        G.vkWaitForFences(dev, 1, &TX.upPending[i].fence, VK_TRUE, ~0ull);
        G.vkDestroyFence(dev, TX.upPending[i].fence, nullptr);
        G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &TX.upPending[i].cb);
    }
    TX.upPending.clear();
    for (size_t i = 0; i < TX.upSlots.size(); ++i) {
        VKTexUpSlot& s = TX.upSlots[i];
        if (s.map) G.vkUnmapMemory(dev, s.mem);
        if (s.mem) G.vkFreeMemory(dev, s.mem, nullptr);
        if (s.buf) G.vkDestroyBuffer(dev, s.buf, nullptr);
    }
    TX.upSlots.clear();
}

unsigned long long vktex_fnv1a(const unsigned char* p, unsigned n)
{
    unsigned long long h = 1469598103934665603ull;
    for (unsigned i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// GeneralsX @feature Qoder 18/10/2026 wedge 4: mirrored images finally get a view. The mirror phase
// (wedge 3) only needed VkImages to prove copies and layout transitions; nothing could sample them,
// because a SHADER_READ_ONLY image without a view and a descriptor binding is a dead resource. Views
// are one-per-image (full mip range, colour aspect) and are destroyed wherever the image is.
VkImageView vktex_make_view(VkDevice dev, VkImage img, VkFormat vkf, unsigned levels, VkImageAspectFlags aspect,
                            VkComponentMapping map = {})
{
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vkf;
    vci.components = map;
    vci.subresourceRange = { aspect, 0, levels, 0, 1 };
    VkImageView v = nullptr;
    if (!chk("CreateImageView(mirror)", G.vkCreateImageView(dev, &vci, nullptr, &v))) return nullptr;
    return v;
}

// Linear lookup is fine at mirror scale (<= 8192 entries); wedge 5 replaces this with a hash and
// descriptor binding. A null key never matches (unkeyed callers always create).
VKTexImg* vktex_find(void* key)
{
    if (!key) return nullptr;
    for (auto& e : TX.imgs) if (e.key == key) return &e;
    return nullptr;
}

// GeneralsX @feature Qoder 19/10/2026 R-02 white-silhouette census. The per-frame shots prove whole
// buildings render as flat white while terrain and trees are fine, and VKNULL=0 says every bound slot
// resolved to a mirror view - so the question is what the mirrored bytes actually ARE. The upload path
// already knows how to read mip0 back (the verify lane); this decodes that readback for the first few
// dozen fresh images per run (GENERALSX_VKWHITE=1) and reports the near-white / near-black texel share,
// naming the texture through the side channel VKTex_Set_Source_Name. White source = engine/loader gave
// white (async load or format bug upstream); clean source + white picture = mirror/shader layer.
void vktex_white_census(const unsigned char* p, unsigned w, unsigned h, VkFormat vkf,
                        unsigned long long& white, unsigned long long& black, unsigned long long& total)
{
    white = black = total = 0;
    struct Acc {
        unsigned long long* w; unsigned long long* b; unsigned long long* t;
        void operator()(unsigned r, unsigned g, unsigned bl) const {
            ++*t;
            if (r >= 240u && g >= 240u && bl >= 240u) ++*w;
            else if (r < 10u && g < 10u && bl < 10u) ++*b;
        }
    } acc = { &white, &black, &total };
    if (vkf == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || vkf == VK_FORMAT_BC2_UNORM_BLOCK || vkf == VK_FORMAT_BC3_UNORM_BLOCK) {
        const unsigned stride = (vkf == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 8u : 16u;
        const unsigned coff = (vkf == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 0u : 8u;
        const unsigned bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
        for (unsigned by = 0; by < bh; ++by) {
            for (unsigned bx = 0; bx < bw; ++bx) {
                const unsigned char* blk = p + ((size_t)by * bw + bx) * stride + coff;
                const unsigned c0 = blk[0] | (blk[1] << 8);
                const unsigned c1 = blk[2] | (blk[3] << 8);
                unsigned col[4][3];
                col[0][0] = ((c0 >> 11) & 31u) * 255u / 31u; col[0][1] = ((c0 >> 5) & 31u) * 255u / 31u; col[0][2] = (c0 & 31u) * 255u / 31u;
                col[1][0] = ((c1 >> 11) & 31u) * 255u / 31u; col[1][1] = ((c1 >> 5) & 31u) * 255u / 31u; col[1][2] = (c1 & 31u) * 255u / 31u;
                const bool four = c0 > c1;
                for (int ch = 0; ch < 3; ++ch) {
                    col[2][ch] = four ? (2u * col[0][ch] + col[1][ch]) / 3u : (col[0][ch] + col[1][ch]) / 2u;
                    col[3][ch] = four ? (col[0][ch] + 2u * col[1][ch]) / 3u : 0u;
                }
                const unsigned long long idx =
                    (unsigned long long)blk[4] | ((unsigned long long)blk[5] << 8) |
                    ((unsigned long long)blk[6] << 16) | ((unsigned long long)blk[7] << 24);
                for (unsigned t = 0; t < 16u; ++t) {
                    const unsigned x = bx * 4u + t % 4u, y = by * 4u + t / 4u;
                    if (x >= w || y >= h) continue;
                    const unsigned sel = (unsigned)((idx >> (t * 2u)) & 3u);
                    if (!four && sel == 3u) continue;   // BC1 reserved-transparent texel
                    acc(col[sel][0], col[sel][1], col[sel][2]);
                }
            }
        }
        return;
    }
    if (vkf == VK_FORMAT_B8G8R8A8_UNORM || vkf == VK_FORMAT_R8G8B8A8_UNORM) {
        const bool bgr = (vkf == VK_FORMAT_B8G8R8A8_UNORM);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const unsigned char* q = p + i * 4;
            acc(bgr ? q[2] : q[0], q[1], bgr ? q[0] : q[2]);
        }
        return;
    }
    if (vkf == VK_FORMAT_R4G4B4A4_UNORM_PACK16) {
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const unsigned v = (unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8);
            acc((v & 15u) * 17u, ((v >> 4) & 15u) * 17u, ((v >> 8) & 15u) * 17u);
        }
        return;
    }
    if (vkf == VK_FORMAT_R5G6B5_UNORM_PACK16) {
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const unsigned v = (unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8);
            acc(((v >> 11) & 31u) * 255u / 31u, ((v >> 5) & 63u) * 255u / 63u, (v & 31u) * 255u / 31u);
        }
        return;
    }
    total = 0;   // format not decoded: caller reports px=0
}
} // anon

void vktex_shutdown(VkDevice dev)
{
    if (dev) {
        for (auto& e : TX.imgs) {
            if (e.view) G.vkDestroyImageView(dev, e.view, nullptr);
            if (e.img) G.vkDestroyImage(dev, e.img, nullptr);
            if (e.mem) G.vkFreeMemory(dev, e.mem, nullptr);
        }
        if (TX.stageMap) G.vkUnmapMemory(dev, TX.stageMem);
        if (TX.stageMem) G.vkFreeMemory(dev, TX.stageMem, nullptr);
        if (TX.stage) G.vkDestroyBuffer(dev, TX.stage, nullptr);
        vktex_shutdown_up_locked(dev);
    }
    TX.imgs.clear();
    TX.vramBytes = 0;
    TX.stage = nullptr; TX.stageMem = nullptr; TX.stageMap = nullptr; TX.stageCap = 0;
}

bool VKTex_Ready()
{
    return g_vulkanCore && g_vulkanCore->IsReady() && vktex_env_on();
}

static bool vktex_upload_2d_locked(unsigned width, unsigned height, unsigned levels,
                                   int ww3d_format, const VKTex_Mip_Source* mips, void* key);

bool VKTex_Upload_2D(void* key, unsigned width, unsigned height, unsigned levels,
                     int ww3d_format, const VKTex_Mip_Source* mips)
{
    if (!VKTex_Ready() || !mips) return false;
    // GeneralsX @feature Qoder 19/10/2026 R-04 timing probe: every mirror upload ends in a full
    // fence wait, so its wall time is the synchronous cost the render thread queues behind while
    // the loader streams textures (and the frame budget in steady play). Accumulate here and let
    // the periodic "tex mirror" line carry the totals.
    auto t0 = std::chrono::steady_clock::now();
    vk_queue_lock();
    bool r = vktex_upload_2d_locked(width, height, levels, ww3d_format, mips, key);
    vk_queue_unlock();
    const long long us = (long long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    TX.upUs += us; ++TX.upCount;
    if (us > TX.upMaxUs) TX.upMaxUs = us;
    return r;
}

// wedge 5: the draw path resolves a mirrored texture to the view it binds into a descriptor slot.
bool VKTex_Query(void* key, VKTex_Info* out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!VKTex_Ready() || !key) return false;
    vk_queue_lock();
    VKTexImg* e = vktex_find(key);
    // GeneralsX @bugfix Qoder 20/10/2026 R-07: the answer must belong to whoever holds this pointer NOW.
    // A mirror whose owner died (or whose address was handed to a newer object) is retired here rather
    // than sampled: the caller gets a miss, which the draw path resolves to the built-in white, and the
    // next upload from the live object rebuilds the mirror under the same key.
    if (e && g_vkIdentify) {
        const unsigned long long live = vk_mirror_identity(key);
        if (e->ident != live) {
            vk_log_ident_drop("tex", key, e->ident, live);
            g_vulkanCore->Retire_Mirror(e->img, e->view, e->mem);
            TX.vramBytes = (TX.vramBytes >= e->size) ? (TX.vramBytes - e->size) : 0ull;
            TX.imgs.erase(TX.imgs.begin() + (long)(e - &TX.imgs[0]));
            e = nullptr;
        }
    }
    if (e) {
        out->image = (void*)e->img;
        out->view = (void*)e->view;
        out->format = (unsigned)e->vkf;
        out->width = e->w; out->height = e->h; out->levels = e->levels;
    }
    vk_queue_unlock();
    return e != nullptr;
}

// GeneralsX @bugfix Qoder 19/10/2026 wedge 11: drop the mirror owned by a dying texture. The lookup key
// is the D3D texture object's raw pointer, and the WW3D allocator hands that pointer to the next texture
// it creates (the sun shadow map landed exactly on the freed address of tsmorningw_nm.tga and inherited
// its upload mirror, so the terrain receiver sampled the normal map instead of the map - the black slab).
// The image may still be bound by a submitted frame, so it goes to the reaper, not to vkDestroyImage.
void VKTex_Drop(void* key)
{
    if (!key || !g_vulkanCore || !g_vulkanCore->IsReady() || !vktex_env_on()) return;
    vk_queue_lock();
    VKTexImg* e = vktex_find(key);
    if (e) {
        g_vulkanCore->Retire_Mirror(e->img, e->view, e->mem);
        // GeneralsX @bugfix Qoder 21/10/2026: keep the budget honest. The image is leaving this table
        // now; the reaper only delays the actual vkFreeMemory by the in-flight window.
        TX.vramBytes = (TX.vramBytes >= e->size) ? (TX.vramBytes - e->size) : 0ull;
        TX.imgs.erase(TX.imgs.begin() + (long)(e - &TX.imgs[0]));
    }
    vk_queue_unlock();
}

static bool vktex_upload_2d_locked(unsigned width, unsigned height, unsigned levels,
                     int ww3d_format, const VKTex_Mip_Source* mips, void* key)
{
    VkDevice dev = g_vulkanCore->Device();

    VKTexFmtInfo f;
    if (!levels || levels > 16 || !width || !height || !vktex_fmt(ww3d_format, f)) { ++TX.fmtSkips; return false; }
    for (unsigned i = 0; i < levels; ++i) if (!mips[i].bits || !mips[i].pitch) { ++TX.fmtSkips; return false; }

    VKTexImg* existing = vktex_find(key);
    unsigned carriedReups = 0;
    const unsigned long long ident = vk_mirror_identity(key);
    if (existing && (existing->w != width || existing->h != height ||
                     existing->levels != levels || existing->vkf != f.vkf ||
                     existing->ident != ident)) {
        // Recycled D3D address: same pointer, different texture. Do NOT copy into this image -
        // release it and fall through to the create path (re-inserted under the same key below).
        // The old image may still be sampled by a frame that is already submitted, so it goes to the
        // reaper instead of being destroyed here (wedge 5 made these images live in real draw graphs).
        // GeneralsX @bugfix Qoder 20/10/2026 R-07: the identity term is the one that catches a reuse the
        // shape guard could see at all - same extent, same format, same level count, different owner.
        g_vulkanCore->Retire_Mirror(existing->img, existing->view, existing->mem);
        TX.vramBytes = (TX.vramBytes >= existing->size) ? (TX.vramBytes - existing->size) : 0ull;
        size_t idx = (size_t)(existing - &TX.imgs[0]);
        // GeneralsX @feature Qoder 19/10/2026 R-04 census: carry the lifetime reupload count through
        // a recycled-address rebuild, else "reup top" restarts at zero every time the allocator
        // reuses the COM pointer and the churn (which is what the census is hunting) never shows.
        carriedReups = existing->reups;
        TX.imgs.erase(TX.imgs.begin() + (long)idx);
        existing = nullptr;
    }
    // GeneralsX @bugfix Qoder 21/10/2026 multi-match exhaustion: both terms describe what is LIVE now
    // (every drop subtracts), so a heavy map load can push the budget over and the following releases
    // bring it back. The image-count ceiling is a guard against pathological churn, not a map budget:
    // one campaign map plus the menu can legitimately keep more than the old 8192 live entries.
    if (!existing && (TX.imgs.size() >= 32768 || TX.vramBytes >= (3ull << 30))) {
        if (!TX.budgetWarned) {
            TX.budgetWarned = true;
            VKWP1_LOG("tex mirror budget hit (32768 live images / 3 GiB live) - skipping new mirrors until releases drop it");
        }
        ++TX.budgetSkips; return false;
    }

    unsigned lw[16], lh[16], off[16], rowBytes[16], rows[16];
    unsigned total = 0;
    for (unsigned i = 0; i < levels; ++i) {
        vktex_dims(width, height, i, lw[i], lh[i]);
        if (f.block)      { rowBytes[i] = ((lw[i] + 3u) / 4u) * f.bytes; rows[i] = (lh[i] + 3u) / 4u; }
        else if (f.expand3) { rowBytes[i] = lw[i] * 4u;                  rows[i] = lh[i]; }
        else              { rowBytes[i] = lw[i] * f.bytes;               rows[i] = lh[i]; }
        unsigned sz = rowBytes[i] * rows[i];
        off[i] = vktex_al16(total);
        total = off[i] + sz;
    }

    // GeneralsX @performance Qoder 19/10/2026 R-04: content-hash dedup for re-uploads. The radar +
    // shroud-mask A4R4G4B4 pair is re-blitted through DX8Wrapper::_Copy_DX8_Rects about 12 times per
    // frame (dyn-mirror census, 19/10 CH02 session: 9536 dynamic mirrors over 776 frames, two stable
    // keys alternating), and every real upload ends in vkWaitForFences on the shared queue, so even
    // unchanged bytes paid a full pipeline drain - 9.3 s of the 60 s run. Hash the source texels
    // (capped at 1 MiB to bound the CPU cost; larger sources store the 0 sentinel = never-skip) and
    // return early when the mirrored image already holds them.
    unsigned long long srcBytes = 0;
    for (unsigned i = 0; i < levels; ++i)
        srcBytes += (unsigned long long)(f.expand3 ? lw[i] * 3u : rowBytes[i]) * rows[i];
    bool hashed = false;
    unsigned long long srcHash = 0;
    if (srcBytes <= (1ull << 20)) {
        hashed = true;
        srcHash = 1469598103934665603ull;
        for (unsigned i = 0; i < levels; ++i) {
            const unsigned span = f.expand3 ? lw[i] * 3u : rowBytes[i];
            for (unsigned r = 0; r < rows[i]; ++r) {
                const unsigned char* s = mips[i].bits + (size_t)r * mips[i].pitch;
                for (unsigned j = 0; j < span; ++j) { srcHash ^= s[j]; srcHash *= 1099511628211ull; }
            }
        }
        if (existing && srcHash == existing->hash) { ++TX.hashSkips; return true; }
    }

    // GeneralsX @feature Qoder 19/10/2026 R-02 white census: force a readback of the first fresh images
    // per run when GENERALSX_VKWHITE=1 and decode what actually landed in the VkImage. The keys match
    // the [VKBIND]/[VKFF] name lines, so a white census names the layer that lost the picture.
    // Capped at 1024x1024 on purpose: census implies a verify readback, and the 8192 terrain atlas
    // (268 MiB mip0) stalled the loader for the whole session when it was included.
    static bool s_whiteGate = vk_env_gate("GENERALSX_VKWHITE");
    static unsigned s_whiteN = 0;
    bool census = false;
    if (existing == nullptr && f.exact && s_whiteGate && s_whiteN < 2048u && width >= 32u && height >= 32u && width <= 1024u && height <= 1024u) {
        census = true; ++s_whiteN;
    }
    bool verify = f.exact && width <= 1024u && height <= 1024u && ((TX.uploads + TX.reuploads + 1) % 512 == 0);
    unsigned mip0Bytes = rowBytes[0] * rows[0];
    unsigned rbOff = vktex_al16(total);
    unsigned long long need = (unsigned long long)rbOff + (verify ? vktex_al16(mip0Bytes) : 0);
    // GeneralsX @performance Qoder 19/10/2026 R-05 wave 9: the upload takes its own ring slot now;
    // TX.stage stays for the synchronous readback/probe users that must wait anyway.
    const int slotIdx = vktex_acquire_up_slot_locked(need);
    if (slotIdx < 0) { ++TX.fmtSkips; return false; }
    VkBuffer sbuf = TX.upSlots[slotIdx].buf;
    unsigned char* sm = (unsigned char*)TX.upSlots[slotIdx].map;
    for (unsigned i = 0; i < levels; ++i) {
        unsigned char* dst = sm + off[i];
        const unsigned char* src = mips[i].bits;
        if (f.rot4) {
            const unsigned words = rowBytes[i] >> 1;
            for (unsigned r = 0; r < rows[i]; ++r) {
                const unsigned char* s = src + (size_t)r * mips[i].pitch;
                unsigned char* d = dst + (size_t)r * rowBytes[i];
                for (unsigned w = 0; w < words; ++w) {
                    unsigned v = (unsigned)s[2 * w] | ((unsigned)s[2 * w + 1] << 8);
                    v = ((v << 4) | (v >> 12)) & 0xFFFFu;
                    d[2 * w] = (unsigned char)(v & 0xFF);
                    d[2 * w + 1] = (unsigned char)(v >> 8);
                }
            }
        } else if (f.expand3) {
            // Three stored bytes in, four out, alpha forced opaque (D3D's R8G8B8 has none).
            for (unsigned r = 0; r < rows[i]; ++r) {
                const unsigned char* s = src + (size_t)r * mips[i].pitch;
                unsigned char* d = dst + (size_t)r * rowBytes[i];
                for (unsigned x = 0; x < lw[i]; ++x) {
                    d[4 * x + 0] = s[3 * x + 0];
                    d[4 * x + 1] = s[3 * x + 1];
                    d[4 * x + 2] = s[3 * x + 2];
                    d[4 * x + 3] = 0xFF;
                }
            }
        } else {
            for (unsigned r = 0; r < rows[i]; ++r) memcpy(dst + (size_t)r * rowBytes[i], src + (size_t)r * mips[i].pitch, rowBytes[i]);
        }
    }

    // GeneralsX @bugfix Qoder 19/10/2026 R-02: the white census reads the packed CPU staging, which is
    // byte-identical to what the copy puts in the image (verify proved that 976/976). Census-on-verify
    // made every censused texture pay a synchronous GPU readback, and 976 of them starved the loader
    // for the whole 61s session (syncUs 55s, never reached the mission). Source-side census is free.
    if (census) {
        unsigned long long wht = 0, blk = 0, tot = 0;
        vktex_white_census(sm + off[0], lw[0], lh[0], f.vkf, wht, blk, tot);
        VKWP1_LOG("VKWHITE new key=%p %ux%u ww3dfmt=%d vkf=%u white%%=%.1f black%%=%.1f px=%llu",
            key, width, height, ww3d_format, (unsigned)f.vkf,
            tot ? 100.0 * (double)wht / (double)tot : 0.0,
            tot ? 100.0 * (double)blk / (double)tot : 0.0, tot);
    }

    // device-local image with dedicated allocation (1.1 core; per-image alloc is fine at mirror scale)
    VkImage img = nullptr;
    VkDeviceMemory mem = nullptr;
    VkMemoryRequirements ireq; ireq.size = 0;
    bool is_new = (existing == nullptr);
    if (is_new) {
        // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: a no-d3d9 run died inside the
        // driver's vkCreateImage with an integer divide (0xC0000094 at a return address on this call). The
        // parameters are the only way to tell which mirror the stub device described badly, so the first
        // creates name themselves before the call.
        static unsigned long s_create_logged = 0;
        if (s_create_logged < 512) {
            ++s_create_logged;
            VKWP1_LOG("mirror create: key=%p %ux%u levels=%u ww3dfmt=%d vkf=%u bytes=%u block=%d pitch0=%u",
                key, width, height, levels, ww3d_format, (unsigned)f.vkf, f.bytes, f.block ? 1 : 0, mips[0].pitch);
        }
        // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: a BC-format image whose extent is
        // not a whole number of 4x4 blocks is what the NVIDIA driver divides by zero over (the no-d3d9 run
        // crashed inside vkCreateImage from the radar mirror). D3D9 hands back compressed surfaces with
        // ragged edges routinely, so name every one instead of dying on an unnamed call.
        if (f.block && ((width % 4u) || (height % 4u))) {
            static unsigned long s_ragged = 0;
            if (++s_ragged <= 32) {
                VKWP1_LOG("block mirror ragged extent #%lu: key=%p %ux%u levels=%u ww3dfmt=%d",
                    s_ragged, key, width, height, levels, ww3d_format);
            }
        }
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = f.vkf;
        ici.extent = { width, height, 1 };
        ici.mipLevels = levels;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!chk("CreateImage(mirror)", G.vkCreateImage(dev, &ici, nullptr, &img))) { ++TX.fmtSkips; return false; }
        G.vkGetImageMemoryRequirements(dev, img, &ireq);
        uint32_t mt = vktex_memtype(ireq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == ~0u) { G.vkDestroyImage(dev, img, nullptr); ++TX.fmtSkips; return false; }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = ireq.size; mai.memoryTypeIndex = mt;
        if (!chk("AllocateMemory(image)", G.vkAllocateMemory(dev, &mai, nullptr, &mem)) ||
            !chk("BindImageMemory(image)", G.vkBindImageMemory(dev, img, mem, 0))) {
            if (mem) G.vkFreeMemory(dev, mem, nullptr);
            G.vkDestroyImage(dev, img, nullptr);
            ++TX.fmtSkips; return false;
        }
    } else {
        img = existing->img; mem = existing->mem;
    }

    // GeneralsX @feature Qoder 19/10/2026 BUG-01 forensics: the SACommandBar black strip is only possible
    // if the alpha byte never reached the mirrored surface (the FF blend chain and vkf mapping were proven
    // correct in the read-only review). The surface claims A8R8G8B8, so sample its alpha row-by-row on the
    // first uploads and print the distribution: all-255 means the loader packed the image opaque and the
    // strip is the texture's own black RGB showing through; a real zero-population moves the breakpoint
    // downstream (sampling/blend). key= matches the mirror-create line for the same texture.
    if (is_new && ww3d_format == 2) {
        static unsigned long s_alpha_probe = 0;
        if (++s_alpha_probe <= 512) {
            const unsigned char* s0 = mips[0].bits;
            unsigned a0 = 0, aff = 0, n = 0, amin = 255, amax = 0;
            for (unsigned y = 0; y < height; y += 2) {
                const unsigned char* row = s0 + (size_t)y * mips[0].pitch;
                for (unsigned x = 0; x < width; x += 2) {
                    const unsigned a = row[4 * x + 3];
                    if (!a) ++a0;
                    if (a == 255) ++aff;
                    if (a < amin) amin = a;
                    if (a > amax) amax = a;
                    ++n;
                }
            }
            VKWP1_LOG("tex alpha probe #%lu: key=%p %ux%u samples=%u a0=%u a255=%u range=%u..%u",
                s_alpha_probe, key, width, height, n, a0, aff, amin, amax);
        }
    }

    // one-shot upload command buffer; barriers double as the mirror-phase layout state machine
    VkCommandBuffer cb = nullptr;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (!chk("AllocCmd(mirror)", G.vkAllocateCommandBuffers(dev, &cai, &cb))) {
        if (is_new) { G.vkFreeMemory(dev, mem, nullptr); G.vkDestroyImage(dev, img, nullptr); }
        ++TX.fmtSkips; return false;
    }
    VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    G.vkBeginCommandBuffer(cb, &bb);
    VkImageSubresourceRange full = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
#define VKTEX_BARRIER(OLD, NEW, SS, SA, DS, DA) do { \
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER }; \
        b.srcAccessMask = (SA); b.dstAccessMask = (DA); b.oldLayout = (OLD); b.newLayout = (NEW); \
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; \
        b.image = img; b.subresourceRange = full; \
        G.vkCmdPipelineBarrier(cb, (SS), (DS), 0, 0, nullptr, 0, nullptr, 1, &b); \
    } while (0)
    // First transition: a fresh image starts UNDEFINED (discard); a re-upload starts from the
    // SHADER_READ_ONLY layout the previous write left it in.
    if (is_new) {
        VKTEX_BARRIER(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    } else {
        VKTEX_BARRIER(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    for (unsigned i = 0; i < levels; ++i) {
        VkBufferImageCopy c = {};
        c.bufferOffset = off[i];
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        c.imageExtent = { lw[i], lh[i], 1 };
        G.vkCmdCopyBufferToImage(cb, sbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    if (verify) {
        VKTEX_BARRIER(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy rc = {};
        rc.bufferOffset = rbOff;
        rc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        rc.imageExtent = { lw[0], lh[0], 1 };
        G.vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sbuf, 1, &rc);
        VKTEX_BARRIER(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
        VKTEX_BARRIER(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
#undef VKTEX_BARRIER
    if (!chk("EndCmd(mirror)", G.vkEndCommandBuffer(cb))) {
        G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
        if (is_new) { G.vkFreeMemory(dev, mem, nullptr); G.vkDestroyImage(dev, img, nullptr); }
        ++TX.fmtSkips; return false;
    }
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = nullptr;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    bool ok = chk("CreateFence(mirror)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
              chk("Submit(mirror)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence));
    if (ok && verify) {
        // the readback bytes must be resident before the host hash compare: this upload waits inline
        ok = chk("Wait(mirror verify)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
        G.vkDestroyFence(dev, fence, nullptr);
        G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
    } else if (ok) {
        // GeneralsX @performance Qoder 19/10/2026 R-05 wave 9: fire-and-forget. Queue order on this
        // one queue already sequences every later draw after this copy, so nothing may observe the
        // image before the transfer lands; only the staging slot and the command buffer have to
        // outlive the GPU, which the pending list + fence reap provides.
        TX.upSlots[slotIdx].busy = true;
        TX.upPending.push_back({ fence, cb, (unsigned)slotIdx });
    } else {
        if (fence) G.vkDestroyFence(dev, fence, nullptr);
        G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
    }
    if (!ok) {
        if (is_new) { G.vkFreeMemory(dev, mem, nullptr); G.vkDestroyImage(dev, img, nullptr); }
        VKWP1_LOG("FAIL tex mirror submit - image dropped"); ++TX.fmtSkips; return false;
    }

    if (verify) {
        unsigned long long hs = vktex_fnv1a(sm + off[0], mip0Bytes);
        unsigned long long hr = vktex_fnv1a(sm + rbOff, mip0Bytes);
        if (hs == hr) ++TX.verifyOK;
        else {
            ++TX.verifyBad;
            VKWP1_LOG("tex mirror VERIFY MISMATCH %ux%u levels=%u ww3dfmt=%d src=%016llx read=%016llx",
                width, height, levels, ww3d_format, hs, hr);
        }
    }
    if (is_new) {
        // GeneralsX @feature Qoder 18/10/2026 wedge 4: every mirrored image now carries the view the
        // descriptor layer binds. A view created while the image sits in SHADER_READ_ONLY is legal -
        // layout is a property of the image at use time, not of the view.
        VkImageView v = vktex_make_view(dev, img, f.vkf, levels, VK_IMAGE_ASPECT_COLOR_BIT, f.map);
        if (!v) { // no view means the image can never be sampled: drop it rather than leak a dead resource
            G.vkFreeMemory(dev, mem, nullptr);
            G.vkDestroyImage(dev, img, nullptr);
            ++TX.fmtSkips;
            return false;
        }
        TX.imgs.push_back({ key, img, mem, width, height, levels, f.vkf, v, hashed ? srcHash : 0ull, carriedReups, ident, ireq.size });
        ++TX.uploads;
        TX.vramBytes += ireq.size;
    } else {
        existing->hash = hashed ? srcHash : 0ull;
        ++existing->reups;
        ++TX.reuploads;
    }
    if (((TX.uploads + TX.reuploads) & 255ull) == 0ull) {
        VKWP1_LOG("tex mirror: uploads=%llu reuploads=%llu hashSkips=%llu vram=%llu MiB fmtSkips=%llu budgetSkips=%llu verify ok=%llu bad=%llu syncUs=%lld/%lld max=%lld ring slots=%zu pending=%zu stalls=%llu",
            TX.uploads, TX.reuploads, TX.hashSkips, TX.vramBytes >> 20, TX.fmtSkips, TX.budgetSkips, TX.verifyOK, TX.verifyBad,
            TX.upUs, TX.upCount, TX.upMaxUs,
            TX.upSlots.size(), TX.upPending.size(), TX.upStalls);
    }
    // GeneralsX @feature Qoder 19/10/2026 R-04 census: after dedup still ~9 real reuploads/frame -
    // name the offenders. Every 1024th reupload prints the three images with the highest lifetime
    // reupload counts (key cross-references "mirror create:" and the [VKFF]/[VKBIND] name lines).
    if ((TX.reuploads & 1023ull) == 0ull) {
        unsigned long long best[3] = {0, 0, 0};
        void* bk[3] = {nullptr, nullptr, nullptr};
        unsigned bw[3] = {0, 0, 0}, bh[3] = {0, 0, 0};
        for (auto& e : TX.imgs) {
            const unsigned long long v = e.reups;
            if (v == 0ull) continue;
            for (int s = 2; s >= 0; --s) {
                if (v > best[s]) {
                    if (s < 2) { best[s + 1] = best[s]; bk[s + 1] = bk[s]; bw[s + 1] = bw[s]; bh[s + 1] = bh[s]; }
                    best[s] = v; bk[s] = e.key; bw[s] = e.w; bh[s] = e.h;
                    break;
                }
            }
        }
        VKWP1_LOG("reup top @%llu: k=%p %ux%u n=%llu | k=%p %ux%u n=%llu | k=%p %ux%u n=%llu",
            TX.reuploads, bk[0], bw[0], bh[0], best[0], bk[1], bw[1], bh[1], best[1], bk[2], bw[2], bh[2], best[2]);
    }
    return true;
}

// ---- wedge 3.4: render-target / depth mirror + image layout state machine -----------------
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.4. See VulkanGeo.h for the scope. The mirror
// phase runs on a queue shared with the texture mirror and the present chain, so every alloc /
// transition / readback that submits takes g_vkQueueCS through the public wrappers below.
namespace {

// One mirror per engine render target / depth texture, keyed by the IDirect3DTexture9*. Same
// recycled-COM-address shape guard as the texture mirror (record the shape that built the image and
// only reuse when it matches). `layout` is the state-machine bookkeeping: it starts UNDEFINED and is
// updated on every barrier, so a caller can ask for a transition and we emit exactly one correct
// barrier (or none, if it is already there).
// Wedge 4 adds the view: an RT mirror that nothing can sample is only half a mirror. The shadow chain,
// the bloom composite and (from wedge 5) the present chain all bind render targets as textures, and the
// aspect follows the image kind (depth mirrors get a DEPTH aspect view).
// GeneralsX @bugfix Qoder 21/10/2026: 'size' mirrors the texture table - RT.vramBytes is live usage,
// subtracted on every replace/drop, so a map's offscreen pair cannot exhaust the budget permanently.
struct VKRTImg { void* key; VkImage img; VkDeviceMemory mem; unsigned w, h; VkFormat vkf; bool depth; VkImageView view; VkImageView sampleView; VkImageLayout layout; unsigned long long ident; unsigned long long size; };

struct VKRTStateT {
    std::vector<VKRTImg> imgs;
    unsigned long long colors = 0, depths = 0, transitions = 0, vramBytes = 0,
                       fmtSkips = 0, budgetSkips = 0, readbacks = 0;
    unsigned long long probeColorOK = 0, probeColorBad = 0, probeDepthOK = 0;
    unsigned long long drawOK = 0, drawBad = 0;
    bool envOn = false, envChecked = false, probeDone = false;
};
VKRTStateT RT;

bool vkrt_env_on()
{
    if (!RT.envChecked) {
        RT.envChecked = true;
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKRT", env, (DWORD)sizeof(env));
        RT.envOn = (n != 0 && env[0] != '0');
    }
    return RT.envOn;
}

// Color render targets must be uncompressed (a BC/DXT image is not COLOR_ATTACHMENT-capable).
bool vkrt_color_fmt(int ww3d, VkFormat& out)
{
    VKTexFmtInfo f;
    if (!vktex_fmt(ww3d, f) || f.block) return false;
    out = f.vkf;
    return true;
}

// Depth ordinal table is the WW3DZFormat enum in ww3dformat.h (see that file). Prefer formats that
// are broadly core-supported; the exotic packed ones map to their nearest usable Vk depth format.
bool vkrt_depth_fmt(int ww3dz, VkFormat& out)
{
    switch (ww3dz) {
    case 1: case 5: out = VK_FORMAT_D16_UNORM; break;          // D16_LOCKABLE / D16
    case 2:         out = VK_FORMAT_D32_SFLOAT; break;         // D32
    case 3:         out = VK_FORMAT_D16_UNORM; break;          // D15S1 -> D16 (1-bit stencil dropped)
    case 4: case 7: out = VK_FORMAT_D24_UNORM_S8_UINT; break;  // D24S8 / D24X4S4
    case 6:         out = VK_FORMAT_X8_D24_UNORM_PACK32; break;// D24X8
    default: return false;
    }
    return true;
}

VkImageLayout vkrt_layout(int l)
{
    switch (l) {
    case VKRT_LAYOUT_UNDEFINED:                 return VK_IMAGE_LAYOUT_UNDEFINED;
    case VKRT_LAYOUT_TRANSFER_DST:              return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case VKRT_LAYOUT_TRANSFER_SRC:              return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case VKRT_LAYOUT_COLOR_ATTACHMENT:          return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case VKRT_LAYOUT_DEPTH_STENCIL_ATTACHMENT:  return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case VKRT_LAYOUT_SHADER_READ_ONLY:          return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case VKRT_LAYOUT_GENERAL:                   return VK_IMAGE_LAYOUT_GENERAL;
    default:                                    return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

// Derive the src/dst stage+access masks for a single from->to layout change. The dst side keys off the
// layout we are entering (what becomes allowed); the src side keys off the layout we are leaving (what
// was previously allowed and must be flushed). Only stage/access correctness matters for sync here;
// the layout values themselves are what the between-barrier ops validate against.
void vkrt_masks(VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags& ss, VkAccessFlags& sa,
                VkPipelineStageFlags& ds, VkAccessFlags& da)
{
    ss = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; sa = 0;
    ds = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; da = 0;
    // leaving state -> what must be made visible first
    switch (from) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:   ss = VK_PIPELINE_STAGE_TRANSFER_BIT;             sa = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:   ss = VK_PIPELINE_STAGE_TRANSFER_BIT;             sa = VK_ACCESS_TRANSFER_READ_BIT;  break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: ss = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; sa = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: ss = (VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT); sa = (VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT); break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: ss = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;    sa = VK_ACCESS_SHADER_READ_BIT;  break;
    case VK_IMAGE_LAYOUT_UNDEFINED:              ss = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;          sa = 0; break;
    default:                                      ss = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;        sa = VK_ACCESS_MEMORY_WRITE_BIT; break;
    }
    // entering state -> what is now permitted
    switch (to) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:   ds = VK_PIPELINE_STAGE_TRANSFER_BIT;             da = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:   ds = VK_PIPELINE_STAGE_TRANSFER_BIT;             da = VK_ACCESS_TRANSFER_READ_BIT;  break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: ds = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; da = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: ds = (VkPipelineStageFlags)(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT); da = (VkAccessFlags)(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT); break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: ds = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;    da = VK_ACCESS_SHADER_READ_BIT;  break;
    default:                                      ds = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;        da = 0; break;
    }
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: a depth format with a stencil component has
// to be described with both aspects in every barrier and view. Naming only DEPTH leaves the stencil half
// of D24S8 out of the layout transition, which is exactly the half the player-colour pass writes.
bool vkrt_fmt_has_stencil(VkFormat f)
{
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT;
}

// Record one layout barrier for 'img' into an existing command buffer (used by the probe).
void vkrt_cmd_barrier(VkCommandBuffer cb, VkImage img, unsigned levels, bool depth, bool stencil,
                      VkImageLayout from, VkImageLayout to)
{
    VkPipelineStageFlags ss, ds; VkAccessFlags sa, da;
    vkrt_masks(from, to, ss, sa, ds, da);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = sa; b.dstAccessMask = da; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    VkImageAspectFlags aspect = depth
        ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
        : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange = { aspect, 0, levels, 0, 1 };
    G.vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Synchronous one-shot barrier for a table image (VKRT_Transition). Caller already holds the queue lock.
bool vkrt_barrier_locked(VKRTImg& e, int to_i)
{
    VkImageLayout to = vkrt_layout(to_i);
    if (to == e.layout) return true;
    VkDevice dev = g_vulkanCore->Device();
    VkCommandBuffer cb = nullptr;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (!chk("AllocCmd(rt)", G.vkAllocateCommandBuffers(dev, &cai, &cb))) return false;
    VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    G.vkBeginCommandBuffer(cb, &bb);
    vkrt_cmd_barrier(cb, e.img, 1, e.depth, vkrt_fmt_has_stencil(e.vkf), e.layout, to);
    if (!chk("EndCmd(rt)", G.vkEndCommandBuffer(cb))) { G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb); return false; }
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = nullptr;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    bool ok = chk("CreateFence(rt)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
              chk("Submit(rt)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
              chk("Wait(rt)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
    if (fence) G.vkDestroyFence(dev, fence, nullptr);
    G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
    if (ok) { e.layout = to; ++RT.transitions; }
    return ok;
}

VKRTImg* vkrt_find(void* key)
{
    if (!key) return nullptr;
    for (auto& e : RT.imgs) if (e.key == key) return &e;
    return nullptr;
}

// Shared create path for color / depth mirrors. depth picks DEPTH_STENCIL_ATTACHMENT usage + a depth
// format; color picks COLOR_ATTACHMENT usage. Reuse on identical shape (recycled-pointer guard);
// destroy + recreate otherwise. Budget 1024 images / 2 GiB for the RT mirror set.
bool vkrt_create_locked(void* key, unsigned w, unsigned h, VkFormat vkf, bool depth)
{
    VkDevice dev = g_vulkanCore->Device();
    if (!w || !h) { ++RT.fmtSkips; return false; }
    VKRTImg* existing = vkrt_find(key);
    const unsigned long long ident = vk_mirror_identity(key);
    if (existing && (existing->w != w || existing->h != h || existing->vkf != vkf ||
                     existing->depth != depth || existing->ident != ident)) {
        // Same shape-guard reason as the texture mirror, and the same reason it now matters: wedge 5
        // renders into these, so a frame in flight can still name this image. The identity term covers the
        // case the shape guard cannot see at all: W3DShaderManager::init re-creates its offscreen pair at
        // the same extent and format on every height-map rebuild, so the new target used to inherit the
        // retired one's bytes and the screen filter read last map's noise.
        g_vulkanCore->Retire_Mirror(existing->img, existing->view, existing->mem);
        if (existing->sampleView && existing->sampleView != existing->view)
            g_vulkanCore->Retire_Mirror(nullptr, existing->sampleView, nullptr);
        RT.vramBytes = (RT.vramBytes >= existing->size) ? (RT.vramBytes - existing->size) : 0ull;
        size_t idx = (size_t)(existing - &RT.imgs[0]);
        RT.imgs.erase(RT.imgs.begin() + (long)idx);
        existing = nullptr;
    }
    if (existing) return true; // already mirrored with this shape
    // Live terms only (replace/drop subtract); the old 1024/2 GiB values were lifetime totals in practice.
    if (RT.imgs.size() >= 4096 || RT.vramBytes >= (2ull << 30)) {
        ++RT.budgetSkips; return false;
    }
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkf;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
              | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = nullptr; VkDeviceMemory mem = nullptr; VkMemoryRequirements ireq = {};
    if (!chk("CreateImage(rt)", G.vkCreateImage(dev, &ici, nullptr, &img))) { ++RT.fmtSkips; return false; }
    G.vkGetImageMemoryRequirements(dev, img, &ireq);
    uint32_t mt = vktex_memtype(ireq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo rtmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    rtmai.allocationSize = ireq.size; rtmai.memoryTypeIndex = mt;
    if (mt == ~0u ||
        !chk("AllocMemory(rt)", G.vkAllocateMemory(dev, &rtmai, nullptr, &mem)) ||
        !chk("BindMemory(rt)", G.vkBindImageMemory(dev, img, mem, 0))) {
        if (mem) G.vkFreeMemory(dev, mem, nullptr);
        G.vkDestroyImage(dev, img, nullptr);
        ++RT.fmtSkips; return false;
    }
    // A depth format that also carries stencil is viewed with both aspects: a framebuffer used with a
    // stencil-test pipeline needs the stencil aspect in the attachment view.
    VkImageAspectFlags viewAspect = depth
        ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT |
            (vkrt_fmt_has_stencil(vkf) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
        : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageView v = vktex_make_view(dev, img, vkf, 1, viewAspect);
    if (!v) {
        G.vkFreeMemory(dev, mem, nullptr);
        G.vkDestroyImage(dev, img, nullptr);
        ++RT.fmtSkips;
        return false;
    }
    // The attachment view above has both aspects, which is what a stencil pipeline needs. The same view
    // cannot also be bound as a sampled image (Vulkan only allows one aspect there), so a depth+stencil
    // mirror gets a second DEPTH-only view for readers. Color mirrors sample through 'v' directly.
    VkImageView sample = v;
    if (depth && (viewAspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        sample = vktex_make_view(dev, img, vkf, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (!sample) {
            G.vkDestroyImageView(dev, v, nullptr);
            G.vkFreeMemory(dev, mem, nullptr);
            G.vkDestroyImage(dev, img, nullptr);
            ++RT.fmtSkips;
            return false;
        }
    }
    RT.imgs.push_back({ key, img, mem, w, h, vkf, depth, v, sample, VK_IMAGE_LAYOUT_UNDEFINED, ident, ireq.size });
    RT.vramBytes += ireq.size;
    if (depth) ++RT.depths; else ++RT.colors;
    return true;
}

// Feed locked SYSTEMMEM RT bytes (top-down, 'pitch' stride) into a device-local color mirror via the
// shared staging buffer, then park it in SHADER_READ_ONLY. Create-or-reuse keyed by the RT pointer.
bool vkrt_readback_locked(void* key, const unsigned char* bits, unsigned pitch,
                          unsigned w, unsigned h, VkFormat vkf)
{
    if (!bits || !pitch || !w || !h) { ++RT.fmtSkips; return false; }
    unsigned rowBytes = w * 4u; // RT mirror readbacks handled here are 32bpp color
    unsigned long long need = (unsigned long long)vktex_al16(rowBytes) * h + 16ull;
    if (!vktex_ensure_stage(need)) { ++RT.fmtSkips; return false; }
    unsigned char* sm = (unsigned char*)TX.stageMap;
    for (unsigned r = 0; r < h; ++r) memcpy(sm + (size_t)r * rowBytes, bits + (size_t)r * pitch, rowBytes);

    if (!vkrt_create_locked(key, w, h, vkf, false)) return false;
    VKRTImg* e = vkrt_find(key);
    if (!e) return false;
    VkDevice dev = g_vulkanCore->Device();

    // one-shot command buffer: current layout -> TRANSFER_DST -> copy -> SHADER_READ_ONLY.
    VkCommandBuffer cb = nullptr;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (!chk("AllocCmd(rtread)", G.vkAllocateCommandBuffers(dev, &cai, &cb))) return false;
    VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    G.vkBeginCommandBuffer(cb, &bb);
    VkImageLayout from = e->layout;
    vkrt_cmd_barrier(cb, e->img, 1, false, false, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {};
    c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.imageExtent = { w, h, 1 };
    G.vkCmdCopyBufferToImage(cb, TX.stage, e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vkrt_cmd_barrier(cb, e->img, 1, false, false, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!chk("EndCmd(rtread)", G.vkEndCommandBuffer(cb))) { G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb); return false; }
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = nullptr;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    bool ok = chk("CreateFence(rtread)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
              chk("Submit(rtread)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
              chk("Wait(rtread)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
    if (fence) G.vkDestroyFence(dev, fence, nullptr);
    G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
    if (!ok) { VKWP1_LOG("FAIL rt readback submit - mirror dropped"); return false; }
    e->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ++RT.readbacks; RT.transitions += 2;
    return true;
}

// One-shot self-test of the layout state machine + readback primitive against this GPU, run at device
// Init when the gate is on (before any engine RT exists). Synthetic integer-format color image goes
// UNDEFINED -> TRANSFER_DST -> (ClearColor) -> TRANSFER_SRC -> (CopyImageToBuffer) -> COLOR_ATTACHMENT
// -> GENERAL; the host bytes are then checked pixel-exact against the clear pattern. A depth image goes
// UNDEFINED -> DEPTH_STENCIL_ATTACHMENT -> (ClearDepthStencil) -> SHADER_READ_ONLY. Frees both after.
void vkrt_probe_locked()
{
    if (RT.probeDone) return;
    RT.probeDone = true;
    VkDevice dev = g_vulkanCore->Device();
    const unsigned P = 64u; // probe edge (small, fixed)

    // ---- color probe ----
    {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UINT;
        ici.extent = { P, P, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = nullptr; VkDeviceMemory mem = nullptr; VkMemoryRequirements req = {};
        if (!chk("probe CreateImage", G.vkCreateImage(dev, &ici, nullptr, &img))) return;
        G.vkGetImageMemoryRequirements(dev, img, &req);
        uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
        if (mt == ~0u || !chk("probe AllocMemory", G.vkAllocateMemory(dev, &mai, nullptr, &mem)) ||
            !chk("probe BindMemory", G.vkBindImageMemory(dev, img, mem, 0))) {
            if (mem) G.vkFreeMemory(dev, mem, nullptr);
            G.vkDestroyImage(dev, img, nullptr); return;
        }
        unsigned long long need = (unsigned long long)vktex_al16(P * 4u) * P + 16ull;
        if (!vktex_ensure_stage(need)) { G.vkFreeMemory(dev, mem, nullptr); G.vkDestroyImage(dev, img, nullptr); return; }

        VkCommandBuffer cb = nullptr;
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        G.vkAllocateCommandBuffers(dev, &cai, &cb);
        VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        G.vkBeginCommandBuffer(cb, &bb);
        vkrt_cmd_barrier(cb, img, 1, false, false, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        // exact integer clear pattern so the host readback compare has no rounding ambiguity
        VkClearColorValue col = {};
        col.uint32[0] = 0x11u; col.uint32[1] = 0x22u; col.uint32[2] = 0x33u; col.uint32[3] = 0x44u;
        VkImageSubresourceRange cc = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        G.vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &cc);
        vkrt_cmd_barrier(cb, img, 1, false, false, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy bic = {};
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bic.imageExtent = { P, P, 1 };
        G.vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TX.stage, 1, &bic);
        // prove we can enter the render-target layout and GENERAL too (state-machine coverage, no op after)
        vkrt_cmd_barrier(cb, img, 1, false, false, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkrt_cmd_barrier(cb, img, 1, false, false, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        bool ended = chk("probe EndCmd", G.vkEndCommandBuffer(cb));
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence = nullptr;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        bool ok = ended &&
                  chk("probe CreateFence", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
                  chk("probe Submit", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
                  chk("probe Wait", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
        if (fence) G.vkDestroyFence(dev, fence, nullptr);
        G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
        unsigned mism = 0;
        if (ok) {
            const unsigned char* sm = (const unsigned char*)TX.stageMap;
            for (unsigned r = 0; r < P; ++r)
                for (unsigned x = 0; x < P; ++x) {
                    const unsigned char* px = sm + (size_t)r * vktex_al16(P * 4u) + (size_t)x * 4u;
                    if (px[0] != 0x11 || px[1] != 0x22 || px[2] != 0x33 || px[3] != 0x44) ++mism;
                }
            RT.transitions += 4;
        }
        if (ok && mism == 0) ++RT.probeColorOK; else ++RT.probeColorBad;
        VKWP1_LOG("VKRT probe color: %s  clear/readback %u x %u  mismatches=%u  transitions=UNDEFINED->TDST->TSRC->COLOR_ATT->GENERAL",
            (ok && mism == 0) ? "PASS" : "FAIL", P, P, mism);
        G.vkFreeMemory(dev, mem, nullptr);
        G.vkDestroyImage(dev, img, nullptr);
    }

    // ---- depth probe (allocation + depth-aspect layout transitions + clear; no host readback) ----
    {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
        ici.extent = { P, P, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = nullptr; VkDeviceMemory mem = nullptr; VkMemoryRequirements req = {};
        if (chk("probe CreateImage(depth)", G.vkCreateImage(dev, &ici, nullptr, &img))) {
            G.vkGetImageMemoryRequirements(dev, img, &req);
            uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
            if (mt != ~0u && chk("probe AllocMemory(depth)", G.vkAllocateMemory(dev, &mai, nullptr, &mem)) &&
                chk("probe BindMemory(depth)", G.vkBindImageMemory(dev, img, mem, 0))) {
                VkCommandBuffer cb = nullptr;
                VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
                G.vkAllocateCommandBuffers(dev, &cai, &cb);
                VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                G.vkBeginCommandBuffer(cb, &bb);
                vkrt_cmd_barrier(cb, img, 1, true, false, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                VkClearDepthStencilValue dv = { 0.25f, 0u };
                VkImageSubresourceRange dc = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                G.vkCmdClearDepthStencilImage(cb, img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, &dv, 1, &dc);
                vkrt_cmd_barrier(cb, img, 1, true, false, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                bool ended = chk("probe EndCmd(depth)", G.vkEndCommandBuffer(cb));
                VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                VkFence fence = nullptr;
                VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                bool ok = ended &&
                          chk("probe CreateFence(depth)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
                          chk("probe Submit(depth)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
                          chk("probe Wait(depth)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
                if (fence) G.vkDestroyFence(dev, fence, nullptr);
                G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
                if (ok) { ++RT.probeDepthOK; RT.transitions += 2; }
                VKWP1_LOG("VKRT probe depth: %s  D32_SFLOAT alloc + UNDEFINED->DEPTH_ATT->READ_ONLY + clear",
                    ok ? "PASS" : "FAIL");
            } else {
                if (mem) G.vkFreeMemory(dev, mem, nullptr);
                VKWP1_LOG("VKRT probe depth: alloc/bind FAIL");
            }
            G.vkDestroyImage(dev, img, nullptr);
        }
    }
    VKWP1_LOG("VKRT probe done: colorOK=%llu colorBad=%llu depthOK=%llu transitions=%llu",
        RT.probeColorOK, RT.probeColorBad, RT.probeDepthOK, RT.transitions);
}

} // anon

// ---- public wedge 3.4 wrappers (all serialize on the shared queue lock) ----
bool VKRT_Ready()
{
    return g_vulkanCore && g_vulkanCore->IsReady() && vkrt_env_on();
}

bool VKRT_Alloc_Color(void* key, unsigned int width, unsigned int height, int ww3d_format)
{
    if (!VKRT_Ready() || !key) return false;
    VkFormat vkf;
    if (!vkrt_color_fmt(ww3d_format, vkf)) { ++RT.fmtSkips; return false; }
    vk_queue_lock();
    bool r = vkrt_create_locked(key, width, height, vkf, false);
    vk_queue_unlock();
    return r;
}

bool VKRT_Alloc_Depth(void* key, unsigned int width, unsigned int height, int ww3d_zformat)
{
    if (!VKRT_Ready() || !key) return false;
    VkFormat vkf;
    if (!vkrt_depth_fmt(ww3d_zformat, vkf)) { ++RT.fmtSkips; return false; }
    vk_queue_lock();
    bool r = vkrt_create_locked(key, width, height, vkf, true);
    vk_queue_unlock();
    return r;
}

bool VKRT_Transition(void* key, int to)
{
    if (!VKRT_Ready() || !key || to < 0 || to >= VKRT_LAYOUT_COUNT) return false;
    vk_queue_lock();
    VKRTImg* e = vkrt_find(key);
    bool r = e ? vkrt_barrier_locked(*e, to) : false;
    vk_queue_unlock();
    return r;
}

bool VKRT_Mirror_Readback(void* key, const unsigned char* bits, unsigned int pitch,
                          unsigned int width, unsigned int height, int ww3d_format)
{
    if (!VKRT_Ready() || !key) return false;
    VkFormat vkf;
    if (!vkrt_color_fmt(ww3d_format, vkf)) { ++RT.fmtSkips; return false; }
    vk_queue_lock();
    bool r = vkrt_readback_locked(key, bits, pitch, width, height, vkf);
    vk_queue_unlock();
    return r;
}

// GeneralsX @bugfix Qoder 21/10/2026 SHADOWRT: GPU copy from a render-target mirror into a texture
// mirror. W3DProjectedShadow::updateTexture bakes each caster into one shared RT and then "copies"
// it into that caster's permanent shadow texture through D3D9 GetRenderTargetData/Surface::Copy.
// Under the pure-Vulkan facade the D3D9 surface store is empty, so that copy hands back zeros and
// every shadow texture mirrored black - the multiplicative decal then darkens its whole quad. The
// baked pixels only exist in the RT's VkImage, so move them image-to-image instead. Called after the
// bake's nested End_Render has submitted the frame, so the one-time submit below is queue-ordered
// after the render that produced the source pixels.
bool VKTex_Copy_RT_To_Texture(void* dst_texture, void* src_render_target,
                              unsigned width, unsigned height)
{
#if defined(SAGE_USE_VULKAN_CORE)
    if (!VKRT_Ready() || !g_vulkanCore || !dst_texture || !src_render_target) return false;
    vk_queue_lock();
    VKTexImg* d = vktex_find(dst_texture);
    VKRTImg* s = vkrt_find(src_render_target);
    bool ok = d && s && d->img && s->img && d->vkf == s->vkf;
    if (!ok) {
        vk_queue_unlock();
        return false;
    }
    if (!width)  width  = d->w;
    if (!height) height = d->h;
    if (width > d->w || height > d->h || width > s->w || height > s->h) {
        vk_queue_unlock();
        return false;
    }

    VkDevice dev = g_vulkanCore->Device();
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_vulkanCore->Pool();
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = nullptr;
    if (!chk("AllocCmd(shadowrt)", G.vkAllocateCommandBuffers(dev, &cai, &cb))) {
        vk_queue_unlock();
        return false;
    }
    VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ok = chk("BeginCmd(shadowrt)", G.vkBeginCommandBuffer(cb, &bb));
    if (ok) {
        const VkImageLayout srcFrom = s->layout;
        vkrt_cmd_barrier(cb, s->img, 1, false, false, srcFrom, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkrt_cmd_barrier(cb, d->img, 1, false, false,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageBlit region = {};
        region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.srcOffsets[0] = { 0, 0, 0 };
        region.srcOffsets[1] = { (int32_t)width, (int32_t)height, 1 };
        region.dstOffsets[0] = { 0, 0, 0 };
        region.dstOffsets[1] = { (int32_t)width, (int32_t)height, 1 };
        G.vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

        vkrt_cmd_barrier(cb, d->img, 1, false, false,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkrt_cmd_barrier(cb, s->img, 1, false, false,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcFrom);
        ok = chk("EndCmd(shadowrt)", G.vkEndCommandBuffer(cb));
    }
    if (ok) {
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence = nullptr;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        ok = chk("Fence(shadowrt)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
             chk("Submit(shadowrt)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
             chk("Wait(shadowrt)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
        if (fence) G.vkDestroyFence(dev, fence, nullptr);
        if (ok) ++RT.readbacks;
    }
    G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
    vk_queue_unlock();
    {
        static int s_shadowRtLog = 0;
        if (s_shadowRtLog < 12) {
            ++s_shadowRtLog;
            fprintf(stderr, "[SHADOWRT] dst=%p src=%p %ux%u fmt=%u ok=%d\n",
                dst_texture, src_render_target, width, height, (unsigned)d->vkf, ok ? 1 : 0);
            fflush(stderr);
        }
    }
    return ok;
#else
    (void)dst_texture; (void)src_render_target; (void)width; (void)height;
    return false;
#endif
}


void VKRT_Run_Probe()
{
    if (!VKRT_Ready()) return;
    vk_queue_lock();
    vkrt_probe_locked();
    vk_queue_unlock();
}

void vkrt_shutdown(VkDevice dev)
{
    if (dev) {
        for (auto& e : RT.imgs) {
            if (e.sampleView && e.sampleView != e.view) G.vkDestroyImageView(dev, e.sampleView, nullptr);
            if (e.view) G.vkDestroyImageView(dev, e.view, nullptr);
            if (e.img) G.vkDestroyImage(dev, e.img, nullptr);
            if (e.mem) G.vkFreeMemory(dev, e.mem, nullptr);
        }
    }
    RT.imgs.clear();
    RT.vramBytes = 0;
}

#if defined(SAGE_USE_VULKAN_CORE)
void VKDiag_Log_Mirror_State(const char* why)
{
    unsigned used = 0u, cap = 0u;
    unsigned long long insertFails = 0ull;
    VKD3D_Live_Stats(&used, &cap, &insertFails);
    VKWP1_LOG("[VKMEM] %s: tex imgs=%zu live=%llu MiB up=%llu reup=%llu skip=%llu | rt imgs=%zu live=%llu MiB skip=%llu | live objects=%u/%u insertFail=%llu",
        (why && *why) ? why : "?",
        TX.imgs.size(), TX.vramBytes >> 20, TX.uploads, TX.reuploads, TX.budgetSkips,
        RT.imgs.size(), RT.vramBytes >> 20, RT.budgetSkips,
        used, cap, insertFails);
}
#endif

// ---- wedge 5 accessors into the render-target mirror ---------------------------------------
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5: VulkanDraw.cpp records real frames against
// these mirrors, so it needs the handles and an in-command-buffer form of the layout transition. The
// structs carry Vulkan objects as void* and formats/layouts as ints, which is what keeps the
// no-<vulkan/vulkan.h> rule of VulkanGeo.h intact.

// Reverse of vkrt_layout(): the int id a caller passes to VKRT_Cmd_Transition for the layout an image is
// already in, so the wedge-5 pass bookkeeping can refresh the state machine after a render pass.
int vkrt_layout_id(VkImageLayout l)
{
    switch ((int)l) {
    case VK_IMAGE_LAYOUT_UNDEFINED:                        return VKRT_LAYOUT_UNDEFINED;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:             return VKRT_LAYOUT_TRANSFER_DST;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:             return VKRT_LAYOUT_TRANSFER_SRC;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:         return VKRT_LAYOUT_COLOR_ATTACHMENT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VKRT_LAYOUT_DEPTH_STENCIL_ATTACHMENT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:         return VKRT_LAYOUT_SHADER_READ_ONLY;
    case VK_IMAGE_LAYOUT_GENERAL:                          return VKRT_LAYOUT_GENERAL;
    default:                                               return VKRT_LAYOUT_UNDEFINED;
    }
}

bool VKRT_Alloc_Color_Format(void* key, unsigned int width, unsigned int height, unsigned int vk_format)
{
    if (!VKRT_Ready() || !key || !width || !height) return false;
    vk_queue_lock();
    bool r = vkrt_create_locked(key, width, height, (VkFormat)vk_format, false);
    vk_queue_unlock();
    return r;
}

bool VKRT_Alloc_Depth_Format(void* key, unsigned int width, unsigned int height, unsigned int vk_format)
{
    if (!VKRT_Ready() || !key || !width || !height) return false;
    vk_queue_lock();
    bool r = vkrt_create_locked(key, width, height, (VkFormat)vk_format, true);
    vk_queue_unlock();
    return r;
}

bool VKRT_Query(void* key, VKRT_Info* out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!VKRT_Ready() || !key) return false;
    vk_queue_lock();
    VKRTImg* e = vkrt_find(key);
    // GeneralsX @bugfix Qoder 20/10/2026 R-07: same identity rule as the texture mirror. The recorder
    // resolves its current target and every sampled RT through this call, so an orphaned mirror here is
    // how one map's noise reached the next map's screen filter.
    if (e && g_vkIdentify) {
        const unsigned long long live = vk_mirror_identity(key);
        if (e->ident != live) {
            vk_log_ident_drop("rt", key, e->ident, live);
            g_vulkanCore->Retire_Mirror(e->img, e->view, e->mem);
            if (e->sampleView && e->sampleView != e->view)
                g_vulkanCore->Retire_Mirror(nullptr, e->sampleView, nullptr);
            RT.vramBytes = (RT.vramBytes >= e->size) ? (RT.vramBytes - e->size) : 0ull;
            RT.imgs.erase(RT.imgs.begin() + (long)(e - &RT.imgs[0]));
            e = nullptr;
        }
    }
    if (e) {
        out->image = (void*)e->img;
        out->view = (void*)e->view;
        out->sample_view = (void*)(e->sampleView ? e->sampleView : e->view);
        out->format = (unsigned)e->vkf;
        out->width = e->w; out->height = e->h;
        out->levels = 1;                      // every mirror is created single-mip
        out->layout = vkrt_layout_id(e->layout);
        out->is_depth = e->depth ? 1 : 0;
    }
    vk_queue_unlock();
    return e != nullptr;
}

// GeneralsX @bugfix Qoder 20/10/2026 R-07: forget an RT key at its owner's real death. The table had no
// such entry point (VKTex_Drop is the only one in the tree), so a render target released by the engine left
// its image registered under a pointer the allocator hands to the next object of the same shape.
void VKRT_Drop(void* key)
{
    if (!key || !g_vulkanCore || !g_vulkanCore->IsReady() || !vkrt_env_on()) return;
    vk_queue_lock();
    VKRTImg* e = vkrt_find(key);
    if (e) {
        g_vulkanCore->Retire_Mirror(e->img, e->view, e->mem);
        if (e->sampleView && e->sampleView != e->view)
            g_vulkanCore->Retire_Mirror(nullptr, e->sampleView, nullptr);
        RT.vramBytes = (RT.vramBytes >= e->size) ? (RT.vramBytes - e->size) : 0ull;
        RT.imgs.erase(RT.imgs.begin() + (long)(e - &RT.imgs[0]));
    }
    vk_queue_unlock();
}

bool VKRT_Cmd_Transition(void* command_buffer, void* key, int to)
{
    if (!VKRT_Ready() || !command_buffer || !key || to < 0 || to >= VKRT_LAYOUT_COUNT) return false;
    // No queue lock here on purpose: this records into a command buffer the caller is building and
    // submits nothing, so it is already serialised by whoever owns the frame.
    VKRTImg* e = vkrt_find(key);
    if (!e) return false;
    const VkImageLayout target = vkrt_layout(to);
    if (target == e->layout) return true;
    vkrt_cmd_barrier((VkCommandBuffer)command_buffer, e->img, 1, e->depth,
                     vkrt_fmt_has_stencil(e->vkf), e->layout, target);
    e->layout = target;
    ++RT.transitions;
    return true;
}

// Refresh the stored layout from outside (wedge 5's render passes transition their attachments
// implicitly through initial/finalLayout, and the state machine has to be told the result or the next
// explicit barrier would be recorded against a stale layout).
void VKRT_Set_Layout(void* key, int to)
{
    if (!key || to < 0 || to >= VKRT_LAYOUT_COUNT) return;
    VKRTImg* e = vkrt_find(key);
    if (e) e->layout = vkrt_layout(to);
}

// ---- wedge 4: descriptors, sampler LUT, constant rings ------------------------------------------
// GeneralsX @feature Qoder 18/10/2026 WP1 wedge 4. See Backend/VulkanDesc.h for the design record.
// This is the layer that turns the wedge-3 mirrors into things a shader can read: views (added to the
// texture and RT records in this round), one engine-wide descriptor set layout, a descriptor pool, a
// sampler cache keyed by the D3DSAMP ordinals the facade already carries, and three host-visible
// constant rings that the shader-constant shadow arrays feed through dynamic offsets.
//
// It lives in this translation unit rather than a new VulkanDesc.cpp on purpose: the dynamically
// loaded entry-point table (G) and the core singleton are file-static here, and the acceptance probe
// has to reach into the texture and RT mirror tables directly. Splitting them out is a wedge-5 sized
// refactor of the loader, not of this layer.
namespace {

struct VKDescRing {
    VkBuffer         buf = nullptr;
    VkDeviceMemory   mem = nullptr;
    void*            map = nullptr;
    unsigned long long cap = 0;
    unsigned long long align = 256;
    unsigned long long cur = 0;
    unsigned long long peak = 0;
    unsigned long long allocs = 0;
    unsigned long long rewinds = 0;
    unsigned long long tooBig = 0;
};

struct VKDescStateT {
    bool   envOn = false, envChecked = false, built = false;
    VkDescriptorSetLayout setLayout = nullptr;
    VkDescriptorPool      pool = nullptr;
    // Sampler LUT: linear key list. The census measured 7 distinct sampler tuples in the menu and the
    // full surface is bounded by the D3DSAMP ordinal cross product the engine actually writes, so a
    // vector scan stays cheaper than hashing until wedge 5 proves otherwise.
    std::vector<unsigned long long> sampKeys;
    std::vector<VkSampler>          sampVals;
    VKDescRing ring[VKDESC_RING_COUNT];
    // 1x1 white fallback, bound wherever a stage has no texture (D3D8 leaves unbound stages reading
    // this rather than faulting, and the fixed-function combiners treat DISABLE as passthrough).
    VkImage      whiteImg = nullptr;
    VkDeviceMemory whiteMem = nullptr;
    VkImageView  whiteView = nullptr;
    // Default sampler for descriptor slots the caller leaves unspecified.
    VkSampler    defSampler = nullptr;
    unsigned long long sets = 0, setsBad = 0, sampCreated = 0, sampReuse = 0, sampBad = 0, binds = 0;
    bool probeDone = false;
};
VKDescStateT D4;

// Stable, unique keys for the probe's synthetic resources. Addresses of statics cannot collide with a
// recycled D3D COM pointer in practice, and using them beats fabricating a magic value.
char g_probeColorKey[8];
char g_probeDepthKey[8];
char g_probeTexKey[8];
char g_ffProbeColorKey[8];   // wedge 6: the rig's own float/half/byte target pair
char g_ffProbeDepthKey[8];
// wedge 7: the combiner rig's target plus its two mirrored textures. Slot 0 is a 2x2 so a projected
// coordinate can be told apart from an unprojected one; slot 1 is 1x1 and the last two slots stay null,
// which the descriptor layer fills with the built-in white.
char g_ffpixColorKey[8];
char g_ffpixTexKey0[8];
char g_ffpixTexKey1[8];

bool vkdesc_env_on()
{
    if (!D4.envChecked) {
        D4.envChecked = true;
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKDESC", env, (DWORD)sizeof(env));
        D4.envOn = (n != 0 && env[0] != '0');
    }
    return D4.envOn;
}

bool vkdesc_build_ring(int i, unsigned long long cap, VkBufferUsageFlags usage, unsigned long long align)
{
    VkDevice dev = g_vulkanCore->Device();
    VKDescRing& r = D4.ring[i];
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = cap;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("CreateBuffer(desc ring)", G.vkCreateBuffer(dev, &bci, nullptr, &r.buf))) { r.buf = nullptr; return false; }
    VkMemoryRequirements req; G.vkGetBufferMemoryRequirements(dev, r.buf, &req);
    uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (mt == ~0u ||
        !chk("AllocMemory(desc ring)", G.vkAllocateMemory(dev, &mai, nullptr, &r.mem)) ||
        !chk("BindMemory(desc ring)", G.vkBindBufferMemory(dev, r.buf, r.mem, 0)) ||
        !chk("MapMemory(desc ring)", G.vkMapMemory(dev, r.mem, 0, cap, 0, &r.map))) {
        if (r.map) { G.vkUnmapMemory(dev, r.mem); r.map = nullptr; }
        if (r.mem) { G.vkFreeMemory(dev, r.mem, nullptr); r.mem = nullptr; }
        G.vkDestroyBuffer(dev, r.buf, nullptr); r.buf = nullptr;
        return false;
    }
    r.cap = cap;
    r.align = align ? align : 16ull;
    memset(r.map, 0, cap);
    return true;
}

// The engine's sampler state is six D3DSAMP ordinals. Translate them once per distinct tuple.
//   D3DTEXTUREADDRESS (d3d8types.h, and identically in d3d9types.h): WRAP=1 MIRROR=2 CLAMP=3 BORDER=4
//                    MIRRORONCE=5 - there is no 0, so 0 means "this shadow word was never written".
//   D3DTEXTUREFILTERTYPE (same headers): NONE=0 POINT=1 LINEAR=2 ANISOTROPIC=3 FLATCUBIC=4 GAUSSIANCUBIC=5.
//                    D3D8 has no combined min/mag/mip ordinal - those are D3D7's, and each of the three
//                    filters is a separate state word here.
// The address modes translate one-to-one. The filters collapse: Vulkan's VkFilter has POINT and LINEAR, so
// anything at or above LINEAR (including ANISOTROPIC, which is LINEAR plus the anisotropic pass, and the two
// unused cubic kernels) magnifies and minifies linearly, and D3DTEXF_ANISOTROPIC is what asks for the pass.
// D3DTSS_MIPFILTER = NONE means mipmap filtering is disabled, which in Vulkan is a sampler that cannot
// leave level 0 (maxLod 0) rather than a nearest-between-levels sampler, and that difference is visible.
// D3D8/D3D9 fixed function always uses transparent black as the border colour, so
// VK_SAMPLER_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK is exact rather than an approximation.
VkSamplerAddressMode vkdesc_addr_mode(unsigned a, bool mirrorClampOK)
{
    switch (a) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5:  return mirrorClampOK ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                  : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // unset (0) or out of range
    }
}

VkSampler vkdesc_make_sampler(unsigned aU, unsigned aV, unsigned aW,
                              unsigned minF, unsigned magF, unsigned mipF, unsigned aniso)
{
    const VkFilter mn = (minF >= 2u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;   // POINT / NONE -> nearest
    const VkFilter mg = (magF >= 2u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const bool wantAniso = (minF == 3u || magF == 3u);
    VkSamplerMipmapMode mm;
    // maxLod carries the "mipmapping is off" case; LINEAR mips need the chain open, so the two decisions
    // are taken together rather than in two places.
    float maxLod = 1000.0f;
    if (mipF == 0u)      { mm = VK_SAMPLER_MIPMAP_MODE_NEAREST; maxLod = 0.0f; }  // NONE: base level only
    else if (mipF == 1u) { mm = VK_SAMPLER_MIPMAP_MODE_NEAREST; }                 // POINT
    else                 { mm = VK_SAMPLER_MIPMAP_MODE_LINEAR;  }                 // LINEAR and above

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = mg;
    si.minFilter = mn;
    si.mipmapMode = mm;
    si.addressModeU = vkdesc_addr_mode(aU, g_vulkanCore->SamplerMirrorClamp());
    si.addressModeV = vkdesc_addr_mode(aV, g_vulkanCore->SamplerMirrorClamp());
    si.addressModeW = vkdesc_addr_mode(aW, g_vulkanCore->SamplerMirrorClamp());
    si.mipLodBias = 0.0f;
    // Anisotropy needs the feature bit, which createDeviceAndQueue only enabled when the device
    // offered it; asking without it is undefined driver behaviour, so degrade to the plain filter.
    si.anisotropyEnable = (VkBool32)(g_vulkanCore->SamplerAnisotropy() && (wantAniso || aniso > 1));
    si.maxAnisotropy = 1.0f;
    if (si.anisotropyEnable) {
        float a = (float)aniso;
        if (wantAniso && a < 2.0f) a = 2.0f;
        float capv = g_vulkanCore->MaxSamplerAnisotropy();
        si.maxAnisotropy = a > capv ? capv : a;
    }
    si.compareEnable = VK_FALSE;
    si.compareOp = VK_COMPARE_OP_NEVER;
    si.minLod = 0.0f;
    si.maxLod = maxLod;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    si.unnormalizedCoordinates = VK_FALSE;
    VkSampler s = nullptr;
    if (!chk("CreateSampler(desc)", G.vkCreateSampler(g_vulkanCore->Device(), &si, nullptr, &s))) return nullptr;
    return s;
}

unsigned long long vkdesc_sampler_key(unsigned aU, unsigned aV, unsigned aW,
                                      unsigned minF, unsigned magF, unsigned mipF, unsigned aniso)
{
    return (unsigned long long)aU | ((unsigned long long)aV << 4) | ((unsigned long long)aW << 8)
         | ((unsigned long long)minF << 12) | ((unsigned long long)magF << 16)
         | ((unsigned long long)mipF << 20) | ((unsigned long long)(aniso > 16 ? 16 : aniso) << 24);
}

// The LUT body, kept lock-free so the acceptance probe (which already holds the queue lock) can drive
// it without relying on CRITICAL_SECTION recursion.
void* vkdesc_sampler_locked(unsigned aU, unsigned aV, unsigned aW,
                            unsigned minF, unsigned magF, unsigned mipF, unsigned aniso)
{
    unsigned long long k = vkdesc_sampler_key(aU, aV, aW, minF, magF, mipF, aniso);
    for (size_t i = 0; i < D4.sampKeys.size(); ++i) {
        if (D4.sampKeys[i] == k) { ++D4.sampReuse; return (void*)D4.sampVals[i]; }
    }
    VkSampler s = vkdesc_make_sampler(aU, aV, aW, minF, magF, mipF, aniso);
    if (s) { D4.sampKeys.push_back(k); D4.sampVals.push_back(s); ++D4.sampCreated; return (void*)s; }
    ++D4.sampBad;
    return nullptr;
}

// 1x1 all-255 image, staged through the texture mirror's own staging buffer so the byte path is the
// same one wedge 3.2 proved. Used for empty descriptor slots.
bool vkdesc_build_white()
{
    VkDevice dev = g_vulkanCore->Device();
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { 1, 1, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!chk("CreateImage(white)", G.vkCreateImage(dev, &ici, nullptr, &D4.whiteImg))) return false;
    VkMemoryRequirements req; G.vkGetImageMemoryRequirements(dev, D4.whiteImg, &req);
    uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (mt == ~0u || !chk("AllocMemory(white)", G.vkAllocateMemory(dev, &mai, nullptr, &D4.whiteMem)) ||
        !chk("BindMemory(white)", G.vkBindImageMemory(dev, D4.whiteImg, D4.whiteMem, 0))) {
        if (D4.whiteMem) G.vkFreeMemory(dev, D4.whiteMem, nullptr);
        G.vkDestroyImage(dev, D4.whiteImg, nullptr);
        D4.whiteMem = nullptr; D4.whiteImg = nullptr;
        return false;
    }
    if (!vktex_ensure_stage(64ull)) { G.vkFreeMemory(dev, D4.whiteMem, nullptr); G.vkDestroyImage(dev, D4.whiteImg, nullptr); D4.whiteMem = nullptr; D4.whiteImg = nullptr; return false; }
    unsigned char* sm = (unsigned char*)TX.stageMap;
    for (int b = 0; b < 4; ++b) sm[b] = 0xFF;
    VkCommandBuffer cb = nullptr;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (!chk("AllocCmd(white)", G.vkAllocateCommandBuffers(dev, &cai, &cb))) return false;
    VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    G.vkBeginCommandBuffer(cb, &bb);
    vkrt_cmd_barrier(cb, D4.whiteImg, 1, false, false, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy cpy = {};
    cpy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cpy.imageExtent = { 1, 1, 1 };
    G.vkCmdCopyBufferToImage(cb, TX.stage, D4.whiteImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);
    vkrt_cmd_barrier(cb, D4.whiteImg, 1, false, false, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool ok = chk("EndCmd(white)", G.vkEndCommandBuffer(cb));
    if (ok) {
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence = nullptr;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        ok = chk("CreateFence(white)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
             chk("Submit(white)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
             chk("Wait(white)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
        if (fence) G.vkDestroyFence(dev, fence, nullptr);
    }
    G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
    if (!ok) return false;
    D4.whiteView = vktex_make_view(dev, D4.whiteImg, VK_FORMAT_B8G8R8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    return D4.whiteView != nullptr;
}

bool vkdesc_build()
{
    if (D4.built) return true;
    VkDevice dev = g_vulkanCore->Device();

    // ---- descriptor set layout: the one engine-wide binding surface ----
    VkDescriptorSetLayoutBinding b[6] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // GeneralsX @feature Qoder 18/10/2026 Wedge 8: eight texture slots (was four) so terrain NM
    // can sample register s4. Existing twins that declare u_tex[4] stay legal: binding a shader
    // array only requires it to fit inside the layout's descriptorCount.
    b[0].descriptorCount = 8; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // GeneralsX @refactor Qoder 18/10/2026 wedge 5.1: the light block is a dynamic UBO, not an SSBO.
    // The mirror phase could bind it once at a fixed offset, but a draw path rewrites lights per batch,
    // and a static-offset descriptor would then have to be reallocated per draw. With a dynamic binding
    // one cached set serves every batch and only the offset moves at bind time. 8 lights x 128B = 1 KiB
    // sits far inside the uniform-buffer budget, so nothing is lost by the change except the std430
    // wording; wedge 6 declares the block as a uniform block in GLSL.
    b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // GeneralsX @feature Qoder 22/10/2026 Forward+ night: binding 4 is the Forward+ light block for
    // programs that share binding 3 with another stage. The PS-only terrain programs are paired with
    // vkff.vert, which reads its fixed-function state from binding 3; a fragment program cannot also
    // read the light block there without feeding the vertex stage the wrong bytes. Binding 4 is a
    // static-offset UBO because the per-frame slot address is fixed for the whole slot (the set cache
    // is per slot), so it adds no dynamic offset to any bind call.
    b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // GeneralsX @feature Qoder 22/10/2026 light shadows: binding 5 samples the strongest dynamic
    // light's own depth map. Always written (white fallback), so a shader may declare it unconditionally.
    b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 6; lci.pBindings = b;
    if (!chk("CreateDescriptorSetLayout", G.vkCreateDescriptorSetLayout(dev, &lci, nullptr, &D4.setLayout))) return false;

    // ---- pool ----
    const unsigned MAXSETS = 4096;
    VkDescriptorPoolSize ps[4] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAXSETS * 8u },
        // Three dynamic uniform bindings per set: vertex constants, pixel constants, lights.
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, MAXSETS * 3u },
        // One static uniform binding per set: the Forward+ block at binding 4.
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAXSETS * 1u },
        // Light-shadow map at binding 5 (one combined image sampler per set).
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAXSETS * 1u },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    // GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 6: poolSizeCount named three sizes while the
    // array holds two - the ring split is vertex/pixel/lights but lights share the DYNAMIC_UNIFORM pool
    // size, so the third entry was a read past the end of ps[] (garbage type + count, and the loader
    // happily consumed it because validation layers are not installed on this host).
    pci.maxSets = MAXSETS; pci.poolSizeCount = 4; pci.pPoolSizes = ps;
    if (!chk("CreateDescriptorPool", G.vkCreateDescriptorPool(dev, &pci, nullptr, &D4.pool))) return false;

    // ---- constant rings ----
    // 4096 B vertex + 3584 B pixel per reservation is the engine's register budget
    // (Vertex_Shader_Constants[256] / Pixel_Shader_Constants[224], both vec4-sized), so an 8 MiB ring
    // holds 2048 frames' worth at the mirror phase's draw rate before it rewinds.
    const unsigned long long ALIGN_U = g_vulkanCore->UniformAlign();
    // The lights ring is a dynamic UBO since wedge 5.1, so it follows the uniform alignment rule too.
    bool ringsOK =
        vkdesc_build_ring(VKDESC_RING_VERTEX, 8ull << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ALIGN_U) &&
        vkdesc_build_ring(VKDESC_RING_PIXEL,  8ull << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ALIGN_U) &&
        vkdesc_build_ring(VKDESC_RING_LIGHTS, 2ull << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ALIGN_U);
    if (!ringsOK) return false;

    if (!vkdesc_build_white()) return false;

    // The default sampler bound into empty descriptor slots: clamp to edge, bilinear, no anisotropy.
    // Under the D3D8 ordinals that is min/mag LINEAR (2) and mip POINT (1) with a strength of 1, which is
    // the combination that reads as "filter inside a level, do not ask for anisotropy".
    D4.defSampler = vkdesc_make_sampler(3, 3, 3, 2, 2, 1, 1);
    if (!D4.defSampler) return false;

    D4.built = true;
    VKWP1_LOG("desc layer ok: setlayout+pool(%u sets), rings vtx=%llu MiB pix=%llu MiB lights=%llu MiB, white view=%p",
        MAXSETS, D4.ring[0].cap >> 20, D4.ring[1].cap >> 20, D4.ring[2].cap >> 20, (void*)D4.whiteView);
    return true;
}

void* vkdesc_reserve_locked(int ring, unsigned size, unsigned long long* offset)
{
    if (!D4.built || ring < 0 || ring >= VKDESC_RING_COUNT || !size) return nullptr;
    VKDescRing& r = D4.ring[ring];
    unsigned long long sz = ((unsigned long long)size + r.align - 1) & ~(r.align - 1);
    if (sz > r.cap) { ++r.tooBig; return nullptr; }
    unsigned long long off = (r.cur + r.align - 1) & ~(r.align - 1);
    if (off + sz > r.cap) { off = 0; ++r.rewinds; }
    r.cur = off + sz;
    if (r.cur > r.peak) r.peak = r.cur;
    ++r.allocs;
    if (offset) *offset = off;
    return (char*)r.map + off;
}

bool vkdesc_alloc_set(VkDescriptorSet* out)
{
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = D4.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &D4.setLayout;
    if (!chk("AllocateDescriptorSets", G.vkAllocateDescriptorSets(g_vulkanCore->Device(), &ai, out))) {
        ++D4.setsBad; return false;
    }
    ++D4.sets;
    return true;
}

bool vkdesc_write_set(VkDescriptorSet set, void* const views[8], void* const samplers[8],
                      const VKDesc_Region* vtx, const VKDesc_Region* pix, const VKDesc_Region* lights)
{
    VkDevice dev = g_vulkanCore->Device();
    // Every slot must be written: an unwritten descriptor in a bound set is undefined behaviour.
    VkDescriptorImageInfo img[8];
    for (int i = 0; i < 8; ++i) {
        VkImageView v = (VkImageView)views[i];
        if (!v) v = D4.whiteView;
        VkSampler s = (VkSampler)samplers[i];
        // A combined-image-sampler descriptor needs a real sampler; empty slots fall back to the
        // default (point/clamp) created with the layer, never to VK_NULL_HANDLE.
        if (!s) s = D4.defSampler;
        img[i] = {};
        img[i].sampler = s;
        img[i].imageView = v;
        img[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet w[6] = {};
    unsigned n = 0;
    w[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w[n].dstSet = set; w[n].dstBinding = 0; w[n].descriptorCount = 8;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[n].pImageInfo = img; ++n;

    VkDescriptorBufferInfo bi[3];
    const VKDesc_Region* regs[3] = { vtx, pix, lights };
    const int ringOf[3] = { VKDESC_RING_VERTEX, VKDESC_RING_PIXEL, VKDESC_RING_LIGHTS };
    const VkDescriptorType typeOf[3] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC };
    for (int i = 0; i < 3; ++i) {
        if (!regs[i] || !regs[i]->bytes) return false;
        VKDescRing& r = D4.ring[ringOf[i]];
        bi[i] = { r.buf, 0, (VkDeviceSize)regs[i]->bytes };
        w[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[n].dstSet = set; w[n].dstBinding = (uint32_t)(1 + i); w[n].descriptorCount = 1;
        w[n].descriptorType = typeOf[i]; w[n].pBufferInfo = &bi[i]; ++n;
    }
    // GeneralsX @feature Qoder 22/10/2026: binding 4 must be written too (unwritten descriptors in a
    // bound set are undefined). The probes' shaders do not declare it; any valid range from the lights
    // ring satisfies the layout, and the draw path overwrites this binding with the real FP block.
    VkDescriptorBufferInfo bfp = { D4.ring[VKDESC_RING_LIGHTS].buf, 0, 16 };
    w[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w[n].dstSet = set; w[n].dstBinding = 4; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[n].pBufferInfo = &bfp; ++n;
    // Binding 5: the light-shadow map. The probes never declare it; the built-in white is a valid
    // descriptor and the draw path overwrites this binding with the real map.
    VkDescriptorImageInfo lsImg = {};
    lsImg.sampler = D4.defSampler;
    lsImg.imageView = D4.whiteView;
    lsImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    w[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w[n].dstSet = set; w[n].dstBinding = 5; w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &lsImg; ++n;
    G.vkUpdateDescriptorSets(dev, n, w, 0, nullptr);
    return true;
}

// ---- the wedge 4 acceptance probe ---------------------------------------------------------------
// A real draw into a keyed RT mirror: offscreen render pass with a depth attachment, PSO built from
// the baked probe SPIR-V, vertex data taken from the wedge-3.1 geometry ring (so the ring is proven
// bindable, not just writable), one descriptor set per draw at two different UBO dynamic offsets, and
// a byte-exact host readback against a CPU reference.
//
// Near-then-far draw order is deliberate: if the depth attachment and the LESS compare work, the later
// far draw cannot overwrite the near pixels. Break depth in any way (no attachment, wrong clear,
// wrong compare op, wrong layout) and the far white wins everywhere, which the compare reports.
struct VKDescProbePix { unsigned char b, g, r, a; };

void vkdesc_probe_locked()
{
    if (D4.probeDone) return;
    D4.probeDone = true;
    if (!D4.built) { VKWP1_LOG("VKDESC probe skipped: descriptor layer not built"); return; }
    if (!vkrt_env_on()) { VKWP1_LOG("VKDESC probe skipped: needs GENERALSX_VKRT for the RT mirror"); return; }

    VkDevice dev = g_vulkanCore->Device();
    const unsigned P = 64u;
    RT.drawOK = RT.drawBad = 0;
    unsigned mismatches = 0, checked = 0, skipped = 0, nearPixels = 0;
    unsigned mgN = 0, black = 0;
    bool lutDedup = false;

    // ---- resources: color + depth RT mirrors, and a 1x1 white texture through the tex mirror ----
    if (!vkrt_create_locked(g_probeColorKey, P, P, VK_FORMAT_B8G8R8A8_UNORM, false) ||
        !vkrt_create_locked(g_probeDepthKey, P, P, VK_FORMAT_D32_SFLOAT, true)) {
        VKWP1_LOG("VKDESC probe FAIL: rt mirror alloc"); return;
    }
    VKRTImg* col = vkrt_find(g_probeColorKey);
    VKRTImg* dep = vkrt_find(g_probeDepthKey);
    if (!col || !dep || !col->view || !dep->view) { VKWP1_LOG("VKDESC probe FAIL: rt views missing"); return; }

    // A white texel through wedge 3.2's own upload path, so the bound view is a mirrored one.
    unsigned char white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    VKTex_Mip_Source src[1] = { { white, 4 } };
    if (!vktex_upload_2d_locked(1, 1, 1, 2, src, g_probeTexKey))
        VKWP1_LOG("VKDESC probe: tex mirror refused the 1x1 white (using the built-in fallback)");
    VKTexImg* wt = vktex_find(g_probeTexKey);
    VkImageView texView = (wt && wt->view) ? wt->view : D4.whiteView;
    if (!texView) { VKWP1_LOG("VKDESC probe FAIL: no white texture view"); return; }

    // ---- vertex data, straight out of the geometry ring ----
    // 5 floats per vertex: position.xy, depth, uv.xy. Two triangles (near, far).
    struct V { float x, y, z, u, v; };
    const unsigned vbytes = (unsigned)(6 * sizeof(V));
    VkBuffer vbuf = nullptr;
    VkDeviceMemory vmem = nullptr;
    void* vmap = nullptr;
    VkDeviceSize voff = 0;
    if (g_vulkanCore->GeoRingUp()) {
        vmap = g_vulkanCore->Geo_Alloc(vbytes, false);
        if (vmap) {
            vbuf = g_vulkanCore->GeoBufferHandle();
            voff = (VkDeviceSize)g_vulkanCore->GeoOffsetOf(vmap);
        }
    }
    if (!vmap) { // ring unavailable or full: a dedicated host buffer keeps the probe meaningful
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = 256; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!chk("CreateBuffer(probe vb)", G.vkCreateBuffer(dev, &bci, nullptr, &vbuf))) return;
        VkMemoryRequirements req; G.vkGetBufferMemoryRequirements(dev, vbuf, &req);
        uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
        if (mt == ~0u || !chk("AllocMemory(probe vb)", G.vkAllocateMemory(dev, &mai, nullptr, &vmem)) ||
            !chk("BindMemory(probe vb)", G.vkBindBufferMemory(dev, vbuf, vmem, 0)) ||
            !chk("MapMemory(probe vb)", G.vkMapMemory(dev, vmem, 0, 256, 0, &vmap))) {
            if (vbuf) { G.vkDestroyBuffer(dev, vbuf, nullptr); vbuf = nullptr; }
            return;
        }
        voff = 0;
    }
    if (!vmap) { VKWP1_LOG("VKDESC probe FAIL: no vertex storage"); return; }
    // Texel-space definitions (v measured from the top row, matching the viewport), then to NDC.
    const float fu = 2.0f / (float)P, fv = 2.0f / (float)P;
    V* vv = (V*)vmap;
    // near triangle: (8,8) (8,40) (24,8), z = 0.1
    vv[0] = { -1.0f + 8.0f * fu, -1.0f + 8.0f * fv, 0.1f, 0.0f, 0.0f };
    vv[1] = { -1.0f + 8.0f * fu, -1.0f + 40.0f * fv, 0.1f, 0.0f, 1.0f };
    vv[2] = { -1.0f + 24.0f * fu, -1.0f + 8.0f * fv, 0.1f, 1.0f, 0.0f };
    // far triangle: must over-cover the whole target whatever the viewport y convention is. The
    // first draft here was (-3,-3) (3,-3) (-3,3), which only covers the half-plane x+y<=0 - the
    // probe then read black across the other 2048 pixels and the gate failed. (-1,-1) (4,-1) (-1,4)
    // clips to x+y<=3, x>=-1, y>=-1; mirrored in y that becomes x-y<=3, x>=-1, y<=1. Every pixel
    // centre of a [-1,1] viewport satisfies both (|x-y| and |x+y| peak at 1.96875 < 3), so the far
    // draw cannot be blamed for a coverage hole again.
    vv[3] = { -1.0f, -1.0f, 0.9f, 0.0f, 0.0f };
    vv[4] = {  4.0f, -1.0f, 0.9f, 4.0f, 0.0f };
    vv[5] = { -1.0f,  4.0f, 0.9f, 0.0f, 4.0f };

    // ---- constants: one vertex region (uv offset 0) and two pixel regions (the two colours) ----
    // Every reservation comes back with the ring offset that doubles as the descriptor's dynamic
    // offset, so the probe exercises the same two-offset-per-draw path wedge 5 will use.
    VKDesc_Region rtx, rpixNear, rpixFar, rlight;
    unsigned long long oVtx = 0, oNear = 0, oFar = 0, oLight = 0;
    void* pVtx   = vkdesc_reserve_locked(VKDESC_RING_VERTEX, 256 * 16, &oVtx);
    void* pNear  = vkdesc_reserve_locked(VKDESC_RING_PIXEL,  224 * 16, &oNear);
    void* pFar   = vkdesc_reserve_locked(VKDESC_RING_PIXEL,  224 * 16, &oFar);
    void* pLight = vkdesc_reserve_locked(VKDESC_RING_LIGHTS, 1024, &oLight);
    if (!pVtx || !pNear || !pFar || !pLight) {
        VKWP1_LOG("VKDESC probe FAIL: constant reservations (vtx=%p pix=%p/%p light=%p)",
            pVtx, pNear, pFar, pLight);
        return;
    }
    rtx.host = pVtx;      rtx.bytes = 256 * 16;      rtx.offset = oVtx;
    rpixNear.host = pNear;  rpixNear.bytes = 224 * 16;  rpixNear.offset = oNear;
    rpixFar.host = pFar;   rpixFar.bytes = 224 * 16;   rpixFar.offset = oFar;
    rlight.host = pLight;  rlight.bytes = 1024;        rlight.offset = oLight;
    // magenta for the near draw, white for the far one. Both are symmetric under a red/blue storage
    // swap, so the compare is byte-exact whichever UNORM order the format resolves to - and both keep
    // the green (centre) channel meaningful: magenta has it at 0, white at 255. A pure green second
    // colour would not have worked: it is *not* symmetric, and the expected bytes then depend on
    // B8G8R8A8 vs R8G8B8A8, which is exactly the ambiguity this probe refuses to encode.
    memset(pVtx, 0, 256 * 16);
    memset(pLight, 0, 1024);
    float* cn = (float*)pNear;  cn[0] = 1.0f; cn[1] = 0.0f; cn[2] = 1.0f; cn[3] = 1.0f;
    float* cf = (float*)pFar;   cf[0] = 1.0f; cf[1] = 1.0f; cf[2] = 1.0f; cf[3] = 1.0f;

    // ---- shader modules, pipeline layout, PSO ----
    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs = nullptr, ps = nullptr;
    smvi.codeSize = sizeof(VKDESC_VERT_SPV); smvi.pCode = VKDESC_VERT_SPV;
    bool modsOK = chk("CreateShaderModule(probe vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &vs));
    smvi.codeSize = sizeof(VKDESC_FRAG_SPV); smvi.pCode = VKDESC_FRAG_SPV;
    modsOK = modsOK && chk("CreateShaderModule(probe ps)", G.vkCreateShaderModule(dev, &smvi, nullptr, &ps));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &D4.setLayout;
    VkPipelineLayout playout = nullptr;
    if (modsOK) modsOK = chk("CreatePipelineLayout(probe)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &playout));

    // offscreen render pass, color + depth. Per the wedge-5 layout rule the attachment finalLayout is
    // what the record keeps, so the state machine never has to guess after a pass.
    VkRenderPass opass = nullptr;
    {
        VkAttachmentDescription att[2] = {};
        att[0].format = VK_FORMAT_B8G8R8A8_UNORM; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        att[1].format = VK_FORMAT_D32_SFLOAT; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref; sub.pDepthStencilAttachment = &dref;
        VkSubpassDependency dep2[2] = {};
        dep2[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep2[0].dstSubpass = 0;
        dep2[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep2[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep2[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep2[1].srcSubpass = 0; dep2[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep2[1].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep2[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep2[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep2[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 2; rpi.pAttachments = att;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        rpi.dependencyCount = 2; rpi.pDependencies = dep2;
        modsOK = modsOK && chk("CreateRenderPass(offscreen)", G.vkCreateRenderPass(dev, &rpi, nullptr, &opass));
    }
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkImageView fbatt[2] = { col->view, dep->view };
    fbi.renderPass = opass; fbi.attachmentCount = 2; fbi.pAttachments = fbatt;
    fbi.width = P; fbi.height = P; fbi.layers = 1;
    VkFramebuffer fb = nullptr;
    modsOK = modsOK && chk("CreateFramebuffer(probe)", G.vkCreateFramebuffer(dev, &fbi, nullptr, &fb));

    VkPipeline pso = nullptr;
    // Declared at function scope: they seed the PSO *and* the command buffer, because both are dynamic
    // states and the record has to set them explicitly.
    VkViewport vp = { 0.0f, 0.0f, (float)P, (float)P, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { P, P } };
    if (modsOK) {
        VkVertexInputBindingDescription vbd = { 0, (uint32_t)sizeof(V), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vad[3] = {
            { 0, 0, VK_FORMAT_R32G32_SFLOAT,  0 },
            { 1, 0, VK_FORMAT_R32_SFLOAT,     8 },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,  12 },
        };
        VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbd;
        vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = vad;
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vpi.viewportCount = 1; vpi.pViewports = &vp; vpi.scissorCount = 1; vpi.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.rasterizerDiscardEnable = VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.depthBiasEnable = VK_FALSE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.blendEnable = VK_FALSE; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        VkPipelineShaderStageCreateInfo stg[2] = {};
        stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = vs; stg[0].pName = "main";
        stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = ps; stg[1].pName = "main";
        gpi.stageCount = 2; gpi.pStages = stg;
        gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
        gpi.layout = playout; gpi.renderPass = opass; gpi.subpass = 0;
        modsOK = chk("CreateGraphicsPipelines(probe)",
                     G.vkCreateGraphicsPipelines(dev, nullptr, 1, &gpi, nullptr, &pso));
    }

    VkDescriptorSet setNear = nullptr, setFar = nullptr;
    if (modsOK) {
        // Sampler LUT exercise: the two tuples below are census-real (D3DSAMP ordinals exactly as the
        // facade's shadow holds them - the census read CLAMP-free trilinear and WRAP + ANISOTROPIC with a
        // LINEAR mip filter). Both draws bind one of them, so the probe proves the LUT hands out samplers
        // that a real descriptor and a real draw accept, not just that vkCreateSampler returns.
        void* sNear = vkdesc_sampler_locked(3, 3, 3, 2, 2, 2, 1);   // CLAMP, LINEAR / LINEAR / LINEAR mip
        void* sFar  = vkdesc_sampler_locked(1, 1, 1, 3, 3, 2, 4);   // WRAP, ANISOTROPIC min+mag, LINEAR mip, aniso 4
        if (!sNear) sNear = (void*)D4.defSampler;
        if (!sFar)  sFar  = (void*)D4.defSampler;
        // Re-ask the LUT for both tuples: identical handles back is the proof that the key packs the
        // whole surface the facade writes and that no second sampler leaks per state change.
        lutDedup = (vkdesc_sampler_locked(3, 3, 3, 2, 2, 2, 1) == sNear) &&
                   (vkdesc_sampler_locked(1, 1, 1, 3, 3, 2, 4) == sFar);
        modsOK = vkdesc_alloc_set(&setNear) && vkdesc_alloc_set(&setFar);
        void* views[8] = { (void*)texView, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        void* sampsNear[8] = { sNear, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        void* sampsFar[8]  = { sFar,  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        if (modsOK) modsOK = vkdesc_write_set(setNear, views, sampsNear, &rtx, &rpixNear, &rlight) &&
                             vkdesc_write_set(setFar,  views, sampsFar,  &rtx, &rpixFar,  &rlight);
    }

    // ---- record + submit ----
    if (modsOK) {
        VkCommandBuffer cb = nullptr;
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        modsOK = chk("AllocCmd(probe)", G.vkAllocateCommandBuffers(dev, &cai, &cb));
        // The readback lands in the shared staging buffer, so make sure it is big enough *before*
        // recording against it: growing it mid-record would leave the command buffer pointing at a
        // destroyed VkBuffer.
        if (modsOK && !vktex_ensure_stage((unsigned long long)vktex_al16(P * 4u) * P + 16ull)) modsOK = false;
        if (modsOK) {
            VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            G.vkBeginCommandBuffer(cb, &bb);
            // The pass owns the attachment layouts from here on (wedge 5 rule), so the machine's
            // recorded value is overwritten with finalLayout right after, not barriered before.
            VkClearValue clr[2];
            clr[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };   // load-clear is black: any drawn pixel differs
            clr[1].depthStencil = { 1.0f, 0 };
            VkRenderPassBeginInfo rpb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpb.renderPass = opass; rpb.framebuffer = fb; rpb.renderArea = { { 0, 0 }, { P, P } };
            rpb.clearValueCount = 2; rpb.pClearValues = clr;
            G.vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);
            G.vkCmdSetViewport(cb, 0, 1, &vp);
            G.vkCmdSetScissor(cb, 0, 1, &sc);
            G.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
            VkDeviceSize offs[1] = { voff };
            G.vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, offs);
            // near first: the far draw that follows must be rejected by the depth test over it.
            // Bindings 1, 2 and 3 are the three dynamic uniform rings (vertex / pixel / lights) since
            // wedge 5.1, so exactly three offsets per bind, in binding order.
            uint32_t dOffNear[3] = { (uint32_t)rtx.offset, (uint32_t)rpixNear.offset, (uint32_t)rlight.offset };
            G.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &setNear, 3, dOffNear);
            ++D4.binds;
            G.vkCmdDraw(cb, 3, 1, 0, 0);
            uint32_t dOffFar[3] = { (uint32_t)rtx.offset, (uint32_t)rpixFar.offset, (uint32_t)rlight.offset };
            G.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &setFar, 3, dOffFar);
            ++D4.binds;
            G.vkCmdDraw(cb, 3, 1, 3, 0);
            G.vkCmdEndRenderPass(cb);
            // readback: the pass left the colour image in TRANSFER_SRC, so the copy needs no barrier
            VkBufferImageCopy cpy = {};
            cpy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            cpy.imageExtent = { P, P, 1 };
            G.vkCmdCopyImageToBuffer(cb, col->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TX.stage, 1, &cpy);
            modsOK = chk("EndCmd(probe)", G.vkEndCommandBuffer(cb));
            if (modsOK) {
                VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                VkFence fence = nullptr;
                VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                modsOK = chk("CreateFence(probe)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
                         chk("Submit(probe)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
                         chk("Wait(probe)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
                if (fence) G.vkDestroyFence(dev, fence, nullptr);
            }
            G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb);
        }
        if (modsOK) {
            // bookkeeping: the pass moved both attachments; record what the layouts actually are.
            col->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            dep->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            ++RT.transitions;
        }
    }

    // ---- CPU reference + compare ----
    // The bounding boxes are diagnostics, not gates: the first run of this probe needed them to tell
    // "the far draw never landed" from "the far draw landed in the wrong colour", and they also pin
    // down the viewport y convention on this driver instead of leaving it to speculation.
    if (modsOK) {
        const unsigned char* rb = (const unsigned char*)TX.stageMap;
        const float ax = 8.0f, ay = 8.0f, bx = 8.0f, by = 40.0f, cx2 = 24.0f, cy2 = 8.0f;
        int mgR0 = -1, mgR1 = -1, mgC0 = -1, mgC1 = -1;
        int whR0 = -1, whR1 = -1, whC0 = -1, whC1 = -1;
        unsigned other = 0;   // black / mgN are function-scope: the gate below reads them
        int rowMinC[64], rowMaxC[64];
        for (unsigned r = 0; r < P && r < 64; ++r) rowMinC[r] = rowMaxC[r] = -1;
        for (unsigned j = 0; j < P; ++j) {
            for (unsigned i = 0; i < P; ++i) {
                float px = (float)i + 0.5f, py = (float)j + 0.5f;
                float e1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                float e2 = (cx2 - bx) * (py - by) - (cy2 - by) * (px - bx);
                float e3 = (ax - cx2) * (py - cy2) - (ay - cy2) * (px - cx2);
                bool pos = (e1 >= 0.0f && e2 >= 0.0f && e3 >= 0.0f);
                bool neg = (e1 <= 0.0f && e2 <= 0.0f && e3 <= 0.0f);
                bool inside = pos || neg;
                // A pixel centre sits exactly on a triangle edge only if an edge function is 0; the
                // integer/half-integer geometry used here makes |e| >= 0.5, so this guard is a
                // belt-and-braces exclusion that must count zero pixels for the gate to be meaningful.
                // 'small' is a macro (rpcndr.h defines it as 'char' and windows.h drags that in), so
                // the extrema of the absolute edge values are named minAbs / maxAbs here.
                float a1 = e1 > -e1 ? e1 : -e1;
                float a2 = e2 > -e2 ? e2 : -e2;
                float a3 = e3 > -e3 ? e3 : -e3;
                float minAbs = a1 < a2 ? a1 : a2; if (a3 < minAbs) minAbs = a3;
                float maxAbs = a1 > a2 ? a1 : a2; if (a3 > maxAbs) maxAbs = a3;
                bool exactEdge = (minAbs < 0.25f && maxAbs > 0.25f);
                if (inside && !exactEdge) ++nearPixels;
                const unsigned char* q = rb + (size_t)(j * P + i) * 4u;
                if (q[0] == 255 && q[1] == 0 && q[2] == 255 && q[3] == 255) {
                    // Full bbox + per-row span, not just the last-scanned pixel: the vertical
                    // orientation of the near triangle is what tells the viewport y convention
                    // (the widest magenta row sits at row 8 for a top-down mapping, at row 56 for the
                    // spec-standard flip), and wedge 6 has to write the projection sign from that fact
                    // rather than from a guess.
                    if (mgR0 < 0) { mgR0 = (int)j; mgR1 = (int)j; mgC0 = (int)i; mgC1 = (int)i; }
                    if ((int)j < mgR0) mgR0 = (int)j;
                    if ((int)j > mgR1) mgR1 = (int)j;
                    if ((int)i < mgC0) mgC0 = (int)i;
                    if ((int)i > mgC1) mgC1 = (int)i;
                    ++mgN;
                    if (rowMinC[j] < 0) rowMinC[j] = (int)i;
                    rowMaxC[j] = (int)i;
                } else if (q[0] == 255 && q[1] == 255 && q[2] == 255 && q[3] == 255) {
                    if (whR0 < 0) { whR0 = (int)j; whC0 = (int)i; }
                    whR1 = (int)j; whC1 = (int)i;
                } else if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0) {
                    ++black;
                } else {
                    ++other;
                }
                if (exactEdge) { ++skipped; continue; }
                unsigned char eb0 = 255;                  // both probe colours carry red and blue
                unsigned char eb1 = inside ? 0 : 255;     // magenta inside, white outside
                unsigned char eb2 = 255;
                unsigned char eb3 = 255;
                ++checked;
                if (q[0] != eb0 || q[1] != eb1 || q[2] != eb2 || q[3] != eb3) ++mismatches;
            }
        }
        int wideRow = -1, wideSpan = -1;
        for (unsigned r = 0; r < P && r < 64; ++r) {
            if (rowMinC[r] < 0) continue;
            int span = rowMaxC[r] - rowMinC[r];
            if (span > wideSpan) { wideSpan = span; wideRow = (int)r; }
        }
        VKWP1_LOG("VKDESC probe readback: magenta n=%u rows %d..%d cols %d..%d widestRow=%d (span %d)  white rows %d..%d cols %d..%d  black=%u other=%u",
            mgN, mgR0, mgR1, mgC0, mgC1, wideRow, wideSpan, whR0, whR1, whC0, whC1, black, other);
    }

    // ---- teardown of the probe-only objects (mirrors stay; they are keyed resources) ----
    if (setNear) G.vkFreeDescriptorSets(dev, D4.pool, 1, &setNear);
    if (setFar) G.vkFreeDescriptorSets(dev, D4.pool, 1, &setFar);
    if (pso) G.vkDestroyPipeline(dev, pso, nullptr);
    if (fb) G.vkDestroyFramebuffer(dev, fb, nullptr);
    if (playout) G.vkDestroyPipelineLayout(dev, playout, nullptr);
    if (opass) G.vkDestroyRenderPass(dev, opass, nullptr);
    if (vs) G.vkDestroyShaderModule(dev, vs, nullptr);
    if (ps) G.vkDestroyShaderModule(dev, ps, nullptr);
    if (vmem) { // only the dedicated fallback buffer is ours to unmap; the ring stays mapped for the core
        G.vkUnmapMemory(dev, vmem);
        G.vkFreeMemory(dev, vmem, nullptr);
        G.vkDestroyBuffer(dev, vbuf, nullptr);
    }

    if (modsOK && mismatches == 0 && skipped == 0 && checked == P * P && nearPixels > 0 &&
        mgN == nearPixels && black == 0 &&
        D4.sampCreated >= 2 && lutDedup && D4.binds >= 2) ++RT.drawOK;
    else ++RT.drawBad;
    VKWP1_LOG("VKDESC probe: drawOK=%llu drawBad=%llu mismatches=%u checked=%u skipped=%u nearPixels=%u resources=%d sets=%llu samplers=%llu reuse=%llu sampBad=%llu dedup=%d binds=%llu ringPeaks=%llu/%llu/%llu rewinds=%llu/%llu/%llu tooBig=%llu/%llu/%llu",
        RT.drawOK, RT.drawBad, mismatches, checked, skipped, nearPixels, (int)modsOK,
        D4.sets, D4.sampCreated, D4.sampReuse, D4.sampBad, (int)lutDedup, D4.binds,
        D4.ring[0].peak, D4.ring[1].peak, D4.ring[2].peak,
        D4.ring[0].rewinds, D4.ring[1].rewinds, D4.ring[2].rewinds,
        D4.ring[0].tooBig, D4.ring[1].tooBig, D4.ring[2].tooBig);
    if (!modsOK) VKWP1_LOG("VKDESC probe FAILED before the compare - see the FAIL line above for which Vk call");
}

// ---- the wedge 6 fixed-function vertex-stage acceptance probe ---------------------------------------
//
// What it proves. vkff.vert is a hand-written replacement for the vertex shader the D3D9 device used to
// generate, so every equation in it is one the port can get subtly wrong and the game can only report
// as "the lighting looks a bit off" many rounds later. This probe draws a synthetic rig through the real
// pipeline - real command buffer, real PSO built from the baked SPIR-V, real descriptor set with the
// packed VKDraw_FF_State at a dynamic offset, real RT mirror, real host readback - and compares what the
// GPU wrote against VKFF_Vertex_Ref, the CPU transcription of the documented D3D9 behaviour.
//
// Why the rig is built the way it is. A rasteriser interpolates varyings with its own fixed-point
// perspective-correct weights, so a per-pixel CPU expectation would have to reproduce that quantisation
// to be compared byte-exactly - and no epsilon that also catches a real lighting bug can absorb it. The
// rig therefore makes every vertex of a band emit the *same* value, which makes interpolation exact:
// each band is one axis-aligned rectangle, and the lights, the eye and the normal are arranged so the
// four corners compute identically. That is what "put the point/spot light and the specular eye on the
// axis through the rectangle's centre and perpendicular to its plane" buys: the corners are equidistant
// from the centre (a rectangle's four corners are concyclic), so distance, NdotL and the cone term are
// the same at all four, and only the specular direction differs - second order, and measured.
//
// The bands differ by the *state*, not by the geometry: light type, attenuation coefficients, cone
// angles, material colour sources, the COLORVERTEX gate, the fog modes, SPECULARENABLE, the FVF-missing
// register defaults, the world/view matrices and the D3DFVF_XYZRHW path. Each one is a branch of
// vkff.vert the game actually uses.
//
// What the probe does not prove, stated plainly: D3DRS_NORMALIZENORMALS is not observable through
// vColor, because normalizing is idempotent and the shader normalizes once more at the end anyway (the
// pre-normalize value only travels in vWorldNormal, which wedge 7's pixel stage reads and this probe's
// does not); the vertex alpha only reaches the readback through the fog channel, because one target is
// cheaper than two; and both sides implement the same documented equations, so where the documentation
// and DXVK disagree (the driver the game ran on for every previous round) the probe passes and the
// visual A/B owns it. That trade is deliberate: the probe's job is to catch a typo, a sign, a swizzle
// or a wrong enum, which is where a hand-written fixed-function pipeline actually dies.
#define FFPROBE_BANDS   10
#define FFPROBE_W       64u
#define FFPROBE_BH      8u
#define FFPROBE_H       (FFPROBE_BANDS * FFPROBE_BH)
#define FFPROBE_VSTRIDE 128u            // eight vec4 attributes: position, normal, two colours, four tex sets
#define FFPROBE_SPREAD_LIMIT 1e-5f      // corner-to-corner deviation the rig must achieve to be exact
#define FFPROBE_EF_MARGIN 0.25f         // pixel centres this close to a triangle edge are skipped

bool vkff_env_on()
{
    static bool checked = false;
    static bool on = false;
    if (!checked) {
        checked = true;
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKFFVERT", env, (DWORD)sizeof(env));
        on = (n != 0 && env[0] != '0');
    }
    return on;
}

// The four D3DFOGMODE ordinals. VulkanFF.cpp declares the same four for its own translation unit, and
// spelling them again here is cheaper than exporting them through the header for one probe.
enum {
    BB_FOG_NONE   = 0,
    BB_FOG_EXP    = 1,
    BB_FOG_EXP2   = 2,
    BB_FOG_LINEAR = 3
};

// The matrices in VKDraw_FF_State are the engine's own bytes, and the shader reads them as a std140
// mat4, i.e. column-major, so element (row r, column c) lives at m[4*c + r] and the product the shader
// evaluates is M * v. These helpers are the same convention, which is the only way the CPU side of the
// probe can hand VKFF_Vertex_Ref the same world/camera positions the GPU computed.
inline void ff_id(float* m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
inline void ff_mul(float* o, const float* a, const float* b)   // o = a * b
{
    float t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[4 * c + k] * b[4 * k + r];
            t[4 * c + r] = s;
        }
    memcpy(o, t, sizeof(t));
}
inline void ff_mulv(float* o, const float* m, const float* v)  // o = m * v (vec4)
{
    float t[4];
    for (int r = 0; r < 4; ++r) {
        float s = 0.0f;
        for (int c = 0; c < 4; ++c) s += m[4 * c + r] * v[c];
        t[r] = s;
    }
    memcpy(o, t, sizeof(t));
}
inline void ff_tr(float* m, float x, float y, float z)         // translation
{
    ff_id(m); m[12] = x; m[13] = y; m[14] = z;
}
inline void ff_rz(float* m, float ang)                        // rotation about the z axis
{
    const float c = cosf(ang), s = sinf(ang);
    ff_id(m);
    m[0] = c;  m[1] = s;  m[2] = 0.0f;
    m[4] = -s; m[5] = c;  m[6] = 0.0f;
}
inline void ff_rotz3(float* d, const float* v, float ang)      // rotate a vec3 about z
{
    const float c = cosf(ang), s = sinf(ang);
    const float t[3] = { c * v[0] - s * v[1], s * v[0] + c * v[1], v[2] };
    d[0] = t[0]; d[1] = t[1]; d[2] = t[2];
}
inline void ff_copy3(float* d, const float* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
inline void ff_set4(float* d, float x, float y, float z, float w) { d[0] = x; d[1] = y; d[2] = z; d[3] = w; }

// One vertex of the rig, in the layout the PSO declares: eight vec4 slots so every attribute is
// R32G32B32A32_SFLOAT and the shader's vec3 normal simply ignores the fourth component. The real engine
// path feeds unorm BGRA vertex colours (VKDraw_Attr type 1); floats are used here because the probe
// needs exact values on both sides, and the vertex stage reads whichever representation the format says.
struct FFProbeVert {
    float pos[4];       // location 0: world position, or screen x,y,z,rhw on the XYZRHW band
    float nrm[4];       // location 1
    float dif[4];       // location 2
    float spe[4];       // location 3
    float tex[4][4];    // locations 4..7
};

struct FFProbeBand {
    VKDraw_FF_State ff;
    float rect[4];                  // left, top, right, bottom in device pixels (right/bottom exclusive)
    float expect[4];                // the CPU reference's rgba, taken at corner 0
    float spread;                   // worst corner-to-corner deviation of those four channels
    unsigned corners;               // self-check: how many corners went into spread
};

// Which attachment format this GPU will actually accept as the probe's colour target, best precision
// first. SAMPLED_IMAGE is queried as well as COLOR_ATTACHMENT because wedge 3.4's mirror creates every
// image with both usages, and vkCreateImage would fail on a format that cannot do what the image asks.
bool ff_fmt_usable(VkFormat f)
{
    VkFormatProperties p = {};
    G.vkGetPhysicalDeviceFormatProperties(g_vulkanCore->Phys(), f, &p);
    const VkFormatFeatureFlags need =
        (VkFormatFeatureFlags)(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    return (p.optimalTilingFeatures & need) == need;
}
unsigned ff_texel_bytes(VkFormat f)
{
    return f == VK_FORMAT_R32G32B32A32_SFLOAT ? 16u : (f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u);
}

// IEEE binary16 -> binary32 without a hardware path: the half-float rung of the ladder stores the
// expected values as they came out of the blend, and the compare needs them as floats.
float ff_half(unsigned short h)
{
    const unsigned sign = (unsigned)(h & 0x8000u) << 16;
    unsigned e = (unsigned)((h >> 10) & 0x1Fu);
    unsigned m = (unsigned)(h & 0x3FFu);
    unsigned bits;
    if (e == 0u) {
        if (m == 0u) bits = sign;
        else {
            e = 113u;                                  // 127 - 14: the subnormal exponent base
            while ((m & 0x400u) == 0u) { m <<= 1; --e; }
            m &= 0x3FFu;
            bits = sign | (e << 23) | (m << 13);
        }
    } else if (e == 0x1Fu) {
        bits = sign | (0xFFu << 23) | (m << 13);       // inf / NaN, kept so the compare reports it
    } else {
        bits = sign | ((e + 112u) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// One channel of one texel, normalised to a float in the same units as the reference. The byte rung is
// BGRA storage order, so the channel-to-index map is not the identity - and getting that map wrong is
// exactly the kind of bug the probe is meant to catch, which is why it is written out per format.
float ff_channel(VkFormat f, const unsigned char* texel, int ch)
{
    if (f == VK_FORMAT_R32G32B32A32_SFLOAT) { float v; memcpy(&v, texel + ch * 4, 4); return v; }
    if (f == VK_FORMAT_R16G16B16A16_SFLOAT) {
        unsigned short s; memcpy(&s, texel + ch * 2, 2);
        return ff_half(s);
    }
    static const int bgraOf[4] = { 2, 1, 0, 3 };       // r, g, b, a -> byte index in B,G,R,A
    return (float)texel[bgraOf[ch]] / 255.0f;
}

// One light slot of the state block, written the way DX8Wrapper::Set_Light writes it: Type verbatim in
// the first component of the attenuation vec4, the travel direction paired with the inner cone angle, and
// the outer cone angle paired with the spot exponent.
void ff_probe_light(VKDraw_FF_State* ff, int i, unsigned type, const float* pos, const float* dir,
                    float theta, float phi, float falloff, float a0, float a1, float a2,
                    const float* amb, const float* dif, const float* spe, float range)
{
    VKDraw_FF_Light& L = ff->lights[i];
    ff_set4(L.type_att, (float)type, a0, a1, a2);
    ff_set4(L.pos_range, pos[0], pos[1], pos[2], range);
    ff_set4(L.dir_theta, dir[0], dir[1], dir[2], theta);
    ff_set4(L.phi_falloff, phi, falloff, 0.0f, 0.0f);
    ff_copy3(L.ambient, amb);  L.ambient[3] = 1.0f;
    ff_copy3(L.diffuse, dif);  L.diffuse[3] = 1.0f;
    ff_copy3(L.specular, spe); L.specular[3] = 1.0f;
}

// Build band 'b': fill the packed state block, write its six vertices, then run the CPU reference at the
// four corners to get the expected colour and prove the rig really is corner-constant (a band that fails
// that self-check is a badly built rig, not a broken shader, and 'spread' is what says which).
bool ff_probe_band(int b, FFProbeBand* out, FFProbeVert* verts)
{
    FFProbeBand& B = *out;
    memset(&B, 0, sizeof(B));
    VKDraw_FF_State& ff = B.ff;

    const float bandTop = (float)(b * FFPROBE_BH);
    const float zc = 0.3f;                    // quad plane: always inside [0,1] so nothing is depth-clipped

    // ---- rig parameters, one set per band -------------------------------------------------------
    float cx = 0.0f, cy = 0.0f, hw = 0.9375f, hh = 0.75f;   // world rect -> device cols 2..62, rows +1..+7
    float rotZ = 0.0f, viewZ = 0.0f;                        // world rotation about z, view translation along z
    float nDir[3] = { 0.0f, 0.0f, 1.0f };                   // the world-space normal the band lights with
    float nScale = 1.0f;                                    // extra length on the local normal (NORMALIZE band)
    bool  rhw = false;
    float eyePos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };           // w = 0 -> the shader's own fallback eye
    unsigned fx = VKFF_FLAG_LIGHTING | VKFF_FLAG_HAS_NORMAL | VKFF_FLAG_HAS_DIFFUSE | VKFF_FLAG_COLORVERTEX;
    unsigned mask = 0u, texc = 0u, fogv = BB_FOG_NONE, fogt = BB_FOG_NONE;
    unsigned sAmb = 0u, sDiff = 0u, sSpe = 0u, sEmi = 0u;
    float vcol1[4] = { 0.05f, 0.90f, 0.20f, 0.70f };        // the D3DFVF_DIFFUSE register
    float vcol2[4] = { 0.20f, 0.70f, 0.35f, 0.90f };        // the D3DFVF_SPECULAR register
    float power = 8.0f;

    ff_id(ff.world); ff_id(ff.view); ff_id(ff.proj);
    // GeneralsX @bugfix Qoder 19/10/2026 BUG-03: the vertex stage now multiplies generator output by the
    // stage's texture matrix whenever TTFF names a count; the rigs declare COUNT2/COUNT3 cases, so their
    // tex_mat must be the identity the device would have held, not the zero the memset left.
    for (unsigned s = 0; s < VKFF_MAX_STAGES; ++s) ff_id(ff.tex_mat[s]);
    ff_set4(ff.mat_diffuse,  0.60f, 0.50f, 0.40f, 1.0f);
    ff_set4(ff.mat_ambient,  0.30f, 0.35f, 0.40f, 1.0f);
    ff_set4(ff.mat_specular, 0.70f, 0.60f, 0.50f, 1.0f);
    ff_set4(ff.mat_emissive, 0.05f, 0.02f, 0.08f, 1.0f);
    ff_set4(ff.mat_power,    power, 0.0f, 0.0f, 0.0f);
    ff_set4(ff.scene_ambient, 0.20f, 0.15f, 0.10f, 0.0f);
    ff_set4(ff.fog_color,     0.50f, 0.50f, 0.50f, 0.0f);
    ff_set4(ff.fog_params,    0.0f, 0.0f, 0.0f, 0.0f);
    ff_set4(ff.tex_factor,    0.0f, 0.0f, 0.0f, 0.0f);

    switch (b) {
    case 0: {
        // Plain directional lighting: NdotL with a tilted light, the scene ambient term, the emissive
        // term, and every colour source reading its own material member. SPECULARENABLE off.
        mask = 1u;
        const float dir[3] = { 0.0f, 0.6f, -0.8f };
        const float on[3]  = { 0.0f, 0.0f, 0.0f };
        const float amb[3] = { 0.10f, 0.05f, 0.02f };
        const float dif[3] = { 0.80f, 0.70f, 0.60f };
        const float spe[3] = { 1.0f, 1.0f, 1.0f };
        ff_probe_light(&ff, 0, 3, on, dir, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, amb, dif, spe, 0.0f);
        break;
    }
    case 1: {
        // Point light on the band axis with a full attenuation curve, plus the specular path against the
        // vertex stage's own fallback eye: n = +z, so viewDir is exactly (0,0,1) at every corner and
        // rdotv reduces to the light's own z term - constant across the quad, and raised to Power.
        // The diffuse colour comes from the vertex register (D3DMCS_COLOR1), so this band also proves
        // SPECULAR_ENABLE and the Blinn exponent are wired to the right state.
        fx |= VKFF_FLAG_HAS_SPECULAR | VKFF_FLAG_SPECULAR_ENABLE;
        mask = 1u; sDiff = 1u;
        const float pos[3] = { 0.0f, 0.0f, zc + 2.0f };
        const float dir[3] = { 0.0f, 0.0f, -1.0f };
        const float amb[3] = { 0.05f, 0.05f, 0.05f };
        const float dif[3] = { 0.50f, 0.55f, 0.60f };
        const float spe[3] = { 0.90f, 0.80f, 0.70f };
        ff_probe_light(&ff, 0, 1, pos, dir, 0.0f, 0.0f, 0.0f, 0.2f, 0.1f, 0.05f, amb, dif, spe, 20.0f);
        break;
    }
    case 2: {
        // Spot whose cone rejects the whole quad: the travel direction is sideways, so the dot with the
        // cone axis is +/-0.42 at the corners while cos(Theta) is 0.955 - and both signs are outside, so
        // the zeroed attenuation really is corner-constant. The light still contributes its ambient, and
        // the ambient colour is read from the *vertex specular* register (D3DMCS_COLOR2), which is the
        // only band anywhere that names that source, so a swizzle in pick_source shows up here alone.
        fx |= VKFF_FLAG_HAS_SPECULAR;
        mask = 1u; sAmb = 2u;
        const float pos[3] = { 0.0f, 0.0f, zc + 2.0f };
        const float dir[3] = { 1.0f, 0.0f, 0.0f };
        const float amb[3] = { 0.15f, 0.10f, 0.05f };
        const float dif[3] = { 0.90f, 0.90f, 0.90f };
        const float spe[3] = { 0.0f, 0.0f, 0.0f };
        ff_probe_light(&ff, 0, 2, pos, dir, 0.3f, 0.3f, 0.0f, 1.0f, 0.0f, 0.0f, amb, dif, spe, 10.0f);
        break;
    }
    case 3: {
        // Spot landing *inside* the cone ramp: the corners see the light from cos^-1(0.8575) = 0.54 rad,
        // between Theta 0.4 and Phi 0.8, so the ramp is 0.7166 and the Falloff exponent squares it.
        // Zero-width cones (what the retail Set_Light installs) collapse to the edge case, so this band
        // is the only thing that tests the general form of the ramp.
        mask = 1u;
        const float pos[3] = { 0.0f, 0.0f, zc + 2.0f };
        const float dir[3] = { 0.0f, 0.0f, -1.0f };
        const float amb[3] = { 0.02f, 0.03f, 0.04f };
        const float dif[3] = { 0.70f, 0.45f, 0.60f };
        const float spe[3] = { 0.0f, 0.0f, 0.0f };
        ff_probe_light(&ff, 0, 2, pos, dir, 0.4f, 0.8f, 2.0f, 0.5f, 0.25f, 0.0f, amb, dif, spe, 12.0f);
        break;
    }
    case 4: {
        // D3DRS_COLORVERTEX off. The four source fields are deliberately set to the *vertex* registers
        // here: with the gate closed they must all resolve to the material members, and if the shader
        // ignored the gate the readback would carry the bright green vertex diffuse instead.
        // Linear vertex fog on top, so the alpha channel tests the (end-depth)/(end-start) form.
        fx = VKFF_FLAG_LIGHTING | VKFF_FLAG_HAS_NORMAL | VKFF_FLAG_HAS_DIFFUSE | VKFF_FLAG_FOG_ENABLE;
        mask = 1u; fogv = BB_FOG_LINEAR;
        sAmb = 1u; sDiff = 1u; sSpe = 2u; sEmi = 2u;
        ff_set4(ff.fog_params, 0.1f, 0.5f, 0.0f, 0.0f);
        const float on[3]  = { 0.0f, 0.0f, 0.0f };
        const float dir[3] = { 0.0f, 0.0f, -1.0f };
        const float amb[3] = { 0.10f, 0.10f, 0.10f };
        const float dif[3] = { 0.70f, 0.60f, 0.50f };
        const float spe[3] = { 0.0f, 0.0f, 0.0f };
        ff_probe_light(&ff, 0, 3, on, dir, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, amb, dif, spe, 0.0f);
        break;
    }
    case 5: {
        // D3DRS_LIGHTING off, which is not black: the diffuse register is the colour, unlit, and neither
        // the material, the lights, the scene ambient nor the emissive term may contribute. Every sprite,
        // UI quad and decal in the game takes this branch, so the light slot is filled with something
        // loud and the material with something else loud to prove none of it leaks in.
        fx = VKFF_FLAG_HAS_NORMAL | VKFF_FLAG_HAS_DIFFUSE | VKFF_FLAG_COLORVERTEX |
             VKFF_FLAG_FOG_ENABLE;
        fogv = BB_FOG_EXP;
        sDiff = 1u;                              // the colour is the vertex register, not the material
        ff_set4(ff.fog_params, 0.0f, 0.0f, 0.7f, 0.0f);
        ff_set4(ff.mat_diffuse,  0.10f, 0.10f, 0.90f, 1.0f);
        ff_set4(ff.mat_emissive, 0.40f, 0.40f, 0.40f, 1.0f);
        ff_set4(ff.scene_ambient, 0.60f, 0.60f, 0.60f, 0.0f);
        mask = 1u;
        const float on[3]  = { 0.0f, 0.0f, 0.0f };
        const float dir[3] = { 0.0f, 0.0f, -1.0f };
        const float amb[3] = { 0.90f, 0.10f, 0.10f };
        const float dif[3] = { 0.90f, 0.10f, 0.10f };
        const float spe[3] = { 0.90f, 0.10f, 0.10f };
        ff_probe_light(&ff, 0, 3, on, dir, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, amb, dif, spe, 0.0f);
        break;
    }
    case 6: {
        // EXP2 fog, a second light slot that contributes ambient only (its NdotL is exactly 0, so the
        // ndotl > 0 guard is what keeps its diffuse out of the sum), and D3DFVF_DIFFUSE *absent* from
        // the layout while the ambient source names the vertex register: the fixed-function default is
        // opaque white, and the garbage actually sitting in the attribute must not reach the output.
        fx |= VKFF_FLAG_FOG_ENABLE;
        fx &= ~VKFF_FLAG_HAS_DIFFUSE;
        mask = 3u; fogv = BB_FOG_EXP2; sAmb = 1u;
        ff_set4(ff.fog_params, 0.0f, 0.0f, 1.2f, 0.0f);
        const float on[3]  = { 0.0f, 0.0f, 0.0f };
        const float d0[3]  = { 0.0f, 0.0f, -1.0f };
        const float d1[3]  = { 1.0f, 0.0f, 0.0f };
        const float a0[3]  = { 0.20f, 0.10f, 0.30f };
        const float a1[3]  = { 0.11f, 0.22f, 0.33f };
        const float df0[3] = { 0.60f, 0.70f, 0.80f };
        const float df1[3] = { 0.95f, 0.05f, 0.05f };
        const float sp[3]  = { 0.0f, 0.0f, 0.0f };
        ff_probe_light(&ff, 0, 3, on, d0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, a0, df0, sp, 0.0f);
        ff_probe_light(&ff, 1, 3, on, d1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, a1, df1, sp, 0.0f);
        break;
    }
    case 7: {
        // The world matrix is a real rotation here: the vertices are pre-rotated so the rectangle lands
        // in the same device pixels as every other band, but the normal is rotated by the shader, and a
        // vertex stage that lit with the unrotated normal would get NdotL 0.3062 instead of 0.2592.
        // SPECULARENABLE is on with an rdotv that comes out negative, which makes this the band that
        // tests the rdotv > 0 guard rather than the specular maths.
        fx |= VKFF_FLAG_HAS_SPECULAR | VKFF_FLAG_SPECULAR_ENABLE | VKFF_FLAG_NORMALIZE;
        rotZ = 0.5235988f;                       // 30 degrees
        nScale = 3.0f;                           // a deliberately non-unit local normal
        nDir[0] = 0.5f; nDir[1] = 0.0f; nDir[2] = 0.8660254f;
        mask = 1u;
        const float on[3]  = { 0.0f, 0.0f, 0.0f };
        const float dir[3] = { 0.7071068f, 0.0f, -0.7071068f };
        const float amb[3] = { 0.08f, 0.04f, 0.06f };
        const float dif[3] = { 0.75f, 0.55f, 0.35f };
        const float spe[3] = { 1.0f, 1.0f, 1.0f };
        ff_probe_light(&ff, 0, 3, on, dir, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, amb, dif, spe, 0.0f);
        break;
    }
    case 8: {
        // Two things at once, both about which space a number is measured in. The view matrix translates
        // by +0.5 on z, so fog must use the camera depth 0.8 and not the world z 0.3 - the two give
        // 0.5 and 1.0 for this ramp, far outside any epsilon. And eye_position carries a real eye placed
        // far off in +y with z equal to the quad's, which makes rdotv exactly zero and kills the
        // specular term; the fallback eye would have made rdotv 1.0 and lit it up.
        fx |= VKFF_FLAG_HAS_SPECULAR | VKFF_FLAG_SPECULAR_ENABLE | VKFF_FLAG_FOG_ENABLE;
        viewZ = 0.5f;
        fogv = BB_FOG_LINEAR;
        ff_set4(ff.fog_params, 0.6f, 1.0f, 0.0f, 0.0f);
        eyePos[0] = 0.0f; eyePos[1] = 400000.0f; eyePos[2] = zc; eyePos[3] = 1.0f;
        mask = 1u;
        const float on[3]  = { 0.0f, 0.0f, 0.0f };
        const float dir[3] = { 0.0f, 0.0f, -1.0f };
        const float amb[3] = { 0.05f, 0.05f, 0.05f };
        const float dif[3] = { 0.50f, 0.60f, 0.70f };
        const float spe[3] = { 0.80f, 0.40f, 0.20f };
        ff_probe_light(&ff, 0, 3, on, dir, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, amb, dif, spe, 0.0f);
        break;
    }
    default: {
        // D3DFVF_XYZRHW: the batch is already in the viewport's pixel space and the fixed-function stage
        // only has to undo the viewport and the rhw. Nothing lights here (LIGHTING off, fog off), because
        // on this path the shader never evaluates world or view at all - pWorld and pView stay at the
        // origin - so the reference is fed zeros to match, and what the band proves is the
        // back-projection arithmetic and nothing else. The rect is inset further than the other bands' so
        // that a viewport read from the wrong place shows up as unclaimed pixels, not as a colour error.
        fx = VKFF_FLAG_HAS_DIFFUSE | VKFF_FLAG_COLORVERTEX | VKFF_FLAG_RHW;
        sDiff = 1u;                              // unlit sprite convention: the vertex register is the colour
        rhw = true;
        break;
    }
    }

    // ---- matrices and the packed flag word ----------------------------------------------------
    ff_rz(ff.world, rotZ);
    ff_tr(ff.view, 0.0f, 0.0f, viewZ);
    ff_id(ff.proj);
    memcpy(ff.eye_position, eyePos, sizeof(ff.eye_position));
    ff_set4(ff.viewport, 0.0f, bandTop, (float)FFPROBE_W, (float)FFPROBE_BH);
    ff.mat_power[0] = power;
    ff.flags[0] = fx;
    ff.flags[1] = mask | (texc << 8) | ((unsigned)fogv << 11) | ((unsigned)fogt << 14) |
                  (sAmb << 17) | (sDiff << 19) | (sSpe << 21) | (sEmi << 23);
    ff.flags[2] = 0u; ff.flags[3] = 0u;
    for (int s = 0; s < (int)VKFF_MAX_STAGES; ++s) {
        ff.stage[s].tci = 0u; ff.stage[s].ttff = 0u;
        ff.stage[s].set = (unsigned)s; ff.stage[s].components = 2u;
    }

    // ---- the rectangle, in device pixels and in world ------------------------------------------
    // Non-RHW: the world rect is the input and the device rect is what the identity proj/view mapping
    // plus the Vulkan top-left flip produces, so the compare region cannot disagree with the rasteriser.
    float rect[4];
    if (rhw) { rect[0] = 4.0f; rect[1] = bandTop + 2.0f; rect[2] = 60.0f; rect[3] = bandTop + 6.0f; }
    else {
        rect[0] = (cx - hw + 1.0f) * 0.5f * (float)FFPROBE_W;
        rect[2] = (cx + hw + 1.0f) * 0.5f * (float)FFPROBE_W;
        rect[1] = bandTop + (1.0f - (cy + hh)) * 0.5f * (float)FFPROBE_BH;
        rect[3] = bandTop + (1.0f - (cy - hh)) * 0.5f * (float)FFPROBE_BH;
    }
    B.rect[0] = rect[0]; B.rect[1] = rect[1]; B.rect[2] = rect[2]; B.rect[3] = rect[3];

    // Corner order: top-left, bottom-left, top-right, bottom-right; two triangles (0,1,2) and (1,3,2).
    // The PSO culls nothing, so the shared diagonal's winding is not a question, and no pixel centre
    // lands on it: with these rectangles the diagonal's implicit equation has an integer left-hand side
    // and a half-integer right-hand side at every pixel centre, so the fill rule never has anything to
    // decide and each compared pixel belongs to exactly one triangle.
    float wx[4], wy[4];
    wx[0] = cx - hw; wy[0] = cy + hh;
    wx[1] = cx - hw; wy[1] = cy - hh;
    wx[2] = cx + hw; wy[2] = cy + hh;
    wx[3] = cx + hw; wy[3] = cy - hh;
    const int order[6] = { 0, 1, 2, 1, 3, 2 };

    // The normal the *reference* is handed is what the shader's own normalize would leave, because the
    // caller owns NORMALIZENORMALS (see VulkanFF.h) and the stage normalizes once more afterwards.
    float nRef[3] = { nDir[0], nDir[1], nDir[2] };
    if ((fx & VKFF_FLAG_NORMALIZE) == 0u) { nRef[0] *= nScale; nRef[1] *= nScale; nRef[2] *= nScale; }

    for (int k = 0; k < 6; ++k) {
        const int c = order[k];
        FFProbeVert& V = verts[k];
        memset(&V, 0, sizeof(V));
        if (rhw) {
            // Screen space: x and y are device pixel coordinates the viewport maps 1:1, z is already in
            // [0,1], and w is 1/rhw - rhw = 1 keeps the perspective divide out of the arithmetic.
            V.pos[0] = (c == 0 || c == 1) ? rect[0] : rect[2];
            V.pos[1] = (c == 0 || c == 2) ? rect[1] : rect[3];
            V.pos[2] = 0.5f;
            V.pos[3] = 1.0f;
        } else {
            const float wp[3] = { wx[c], wy[c], zc };
            float lp[3];
            ff_rotz3(lp, wp, -rotZ);               // world = Rz(rotZ), so local = Rz(-rotZ) * world
            V.pos[0] = lp[0]; V.pos[1] = lp[1]; V.pos[2] = zc; V.pos[3] = 1.0f;
            const float nloc[3] = { nDir[0] * nScale, nDir[1] * nScale, nDir[2] * nScale };
            float nn[3];
            ff_rotz3(nn, nloc, -rotZ);
            V.nrm[0] = nn[0]; V.nrm[1] = nn[1]; V.nrm[2] = nn[2]; V.nrm[3] = 0.0f;
        }
        memcpy(V.dif, vcol1, sizeof(V.dif));
        memcpy(V.spe, vcol2, sizeof(V.spe));
        for (int t = 0; t < 4; ++t) ff_set4(V.tex[t], 0.25f * (float)t, 0.5f, 0.0f, 1.0f);
    }

    // ---- the four corners through the CPU reference -------------------------------------------
    float spread = 0.0f;
    for (int c = 0; c < 4; ++c) {
        VKFF_Vertex_In in;
        VKFF_Vertex_Out o;
        memset(&in, 0, sizeof(in));
        if (rhw) {
            // The stage never evaluates world or view on this path, so neither may the reference.
            ff_set4(in.world, 0.0f, 0.0f, 0.0f, 1.0f);
            ff_set4(in.view, 0.0f, 0.0f, 0.0f, 1.0f);
        } else {
            in.world[0] = wx[c]; in.world[1] = wy[c]; in.world[2] = zc;
            in.view[0] = wx[c];  in.view[1] = wy[c];  in.view[2] = zc + viewZ;
        }
        in.normal[0] = nRef[0]; in.normal[1] = nRef[1]; in.normal[2] = nRef[2];
        memcpy(in.diffuse, vcol1, sizeof(in.diffuse));
        memcpy(in.specular, vcol2, sizeof(in.specular));
        VKFF_Vertex_Ref(ff, in, &o);
        const float v[4] = { o.color[0] + o.specular[0], o.color[1] + o.specular[1],
                             o.color[2] + o.specular[2], o.fog };
        if (c == 0) memcpy(B.expect, v, sizeof(v));
        for (int ch = 0; ch < 4; ++ch) {
            const float d = v[ch] - B.expect[ch];
            const float a = d > 0.0f ? d : -d;
            if (a > spread) spread = a;
        }
        ++B.corners;
    }
    B.spread = spread;

    // A band whose corners are not constant is a badly built rig, and saying so here keeps "the probe is
    // not measurable" from being reported as "the shader is wrong".
    if (B.spread > FFPROBE_SPREAD_LIMIT) {
        VKWP1_LOG("VKFF probe band %d: rig not corner-constant (spread %.3e) - expect[%.5f %.5f %.5f %.5f]",
            b, B.spread, B.expect[0], B.expect[1], B.expect[2], B.expect[3]);
        return false;
    }
    return true;
}

// The wedge-6 acceptance probe proper. Runs under the queue lock, once.
void vkff_probe_locked()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (!D4.built) { VKWP1_LOG("VKFF probe skipped: descriptor layer not built"); return; }
    if (!vkrt_env_on()) { VKWP1_LOG("VKFF probe skipped: needs GENERALSX_VKRT for the RT mirror"); return; }

    VkDevice dev = g_vulkanCore->Device();

    static_assert(sizeof(VKDraw_FF_State) == VKFF_STATE_BYTES,
                  "the FF state block is a three-way contract: this struct, the packer, and vkff.vert");

    // ---- the format ladder --------------------------------------------------------------------
    // Full float first, because that is the only rung where a wrong lighting equation cannot hide behind
    // 8-bit quantisation; half-float next; the byte target last, which still catches every enum, swizzle
    // and sign error but not a small numerical drift.
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    float eps = 0.0f;
    const char* fmtName = "none";
    struct FFProbeFmt { VkFormat f; float eps; const char* name; } ladder[3] = {
        { VK_FORMAT_R32G32B32A32_SFLOAT, 2e-3f,  "R32G32B32A32_SFLOAT" },
        { VK_FORMAT_R16G16B16A16_SFLOAT, 1e-2f,  "R16G16B16A16_SFLOAT" },
        { VK_FORMAT_B8G8R8A8_UNORM,      1.5f / 255.0f, "B8G8R8A8_UNORM" },
    };
    bool byteFmt = false;
    for (int i = 0; i < 3; ++i) {
        if (!ff_fmt_usable(ladder[i].f)) continue;
        fmt = ladder[i].f; eps = ladder[i].eps; fmtName = ladder[i].name;
        byteFmt = (i == 2);
        break;
    }
    if (fmt == VK_FORMAT_UNDEFINED) { VKWP1_LOG("VKFF probe FAIL: no usable colour attachment format"); return; }
    const unsigned texel = ff_texel_bytes(fmt);

    // ---- the rig -------------------------------------------------------------------------------
    static FFProbeBand bands[FFPROBE_BANDS];
    static FFProbeVert bandVerts[FFPROBE_BANDS][6];
    bool rigOK = true;
    for (int b = 0; b < FFPROBE_BANDS; ++b) {
        if (!ff_probe_band(b, &bands[b], bandVerts[b])) rigOK = false;
    }
    if (!rigOK) { VKWP1_LOG("VKFF probe FAILED before the draw: one or more rigs are not corner-constant"); return; }

    // ---- target, vertex storage, constants ------------------------------------------------------
    if (!vkrt_create_locked(g_ffProbeColorKey, FFPROBE_W, FFPROBE_H, fmt, false)) {
        VKWP1_LOG("VKFF probe FAIL: rt mirror alloc (%s %ux%u)", fmtName, FFPROBE_W, FFPROBE_H);
        return;
    }
    VKRTImg* col = vkrt_find(g_ffProbeColorKey);
    if (!col || !col->view) { VKWP1_LOG("VKFF probe FAIL: rt view missing"); return; }

    const unsigned vbytes = FFPROBE_BANDS * 6u * FFPROBE_VSTRIDE;
    VkBuffer vbuf = nullptr; VkDeviceMemory vmem = nullptr; void* vmap = nullptr; VkDeviceSize voff = 0;
    if (g_vulkanCore->GeoRingUp()) {
        vmap = g_vulkanCore->Geo_Alloc(vbytes, false);
        if (vmap) {
            vbuf = (VkBuffer)g_vulkanCore->GeoBufferHandle();
            voff = (VkDeviceSize)g_vulkanCore->GeoOffsetOf(vmap);
        }
    }
    if (!vmap) { // ring unavailable or full: a dedicated host buffer keeps the probe meaningful
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = vbytes; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!chk("CreateBuffer(ff probe vb)", G.vkCreateBuffer(dev, &bci, nullptr, &vbuf))) return;
        VkMemoryRequirements req; G.vkGetBufferMemoryRequirements(dev, vbuf, &req);
        uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
        if (mt == ~0u ||
            !chk("AllocMemory(ff probe vb)", G.vkAllocateMemory(dev, &mai, nullptr, &vmem)) ||
            !chk("BindMemory(ff probe vb)", G.vkBindBufferMemory(dev, vbuf, vmem, 0)) ||
            !chk("MapMemory(ff probe vb)", G.vkMapMemory(dev, vmem, 0, vbytes, 0, &vmap))) {
            if (vmem) { G.vkUnmapMemory(dev, vmem); G.vkFreeMemory(dev, vmem, nullptr); }
            if (vbuf) { G.vkDestroyBuffer(dev, vbuf, nullptr); }
            return;
        }
        voff = 0;
    }
    if (!vmap) { VKWP1_LOG("VKFF probe FAIL: no vertex storage"); return; }
    // Geo_Alloc already hands back the mapped pointer of the reservation: voff is that same position
    // expressed as a device offset for vkCmdBindVertexBuffers, so the host-side write base is vmap
    // alone. Adding voff on top shifted every band two vertices (256B) past where the GPU looked, and
    // the probe drew each triangle from the previous band's tail - a missing corner per band.
    unsigned char* vbase = (unsigned char*)vmap;
    for (int b = 0; b < FFPROBE_BANDS; ++b)
        memcpy(vbase + (size_t)b * 6u * FFPROBE_VSTRIDE, bandVerts[b], 6u * FFPROBE_VSTRIDE);

    // One vertex-constant and one pixel-constant region for the whole probe (the fixed-function stage
    // reads neither; they exist because the set layout demands all four bindings be filled), and one
    // FF region per band - which is what makes one descriptor set serve ten draws with ten different
    // lighting states: only the dynamic offset of binding 3 moves between them.
    const unsigned VTX_BYTES = 256u * 16u, PIX_BYTES = 224u * 16u;
    VKDesc_Region rtx, rpx, rff;
    unsigned long long oVtx = 0, oPix = 0, oFF[FFPROBE_BANDS] = { 0 };
    void* pVtx = vkdesc_reserve_locked(VKDESC_RING_VERTEX, VTX_BYTES, &oVtx);
    void* pPix = vkdesc_reserve_locked(VKDESC_RING_PIXEL, PIX_BYTES, &oPix);
    void* pFF[FFPROBE_BANDS] = { nullptr };
    for (int b = 0; b < FFPROBE_BANDS; ++b) pFF[b] = vkdesc_reserve_locked(VKDESC_RING_LIGHTS, VKFF_STATE_BYTES, &oFF[b]);
    if (!pVtx || !pPix) { VKWP1_LOG("VKFF probe FAIL: vtx/pix constant reservations"); return; }
    for (int b = 0; b < FFPROBE_BANDS; ++b) {
        if (!pFF[b]) { VKWP1_LOG("VKFF probe FAIL: FF ring reservation for band %d (lights ring peak %llu)",
                                 b, D4.ring[VKDESC_RING_LIGHTS].peak); return; }
        memcpy(pFF[b], &bands[b].ff, VKFF_STATE_BYTES);
    }
    memset(pVtx, 0, VTX_BYTES);
    memset(pPix, 0, PIX_BYTES);
    rtx.host = pVtx; rtx.bytes = VTX_BYTES; rtx.offset = oVtx;
    rpx.host = pPix; rpx.bytes = PIX_BYTES; rpx.offset = oPix;

    // ---- modules, layout, pass, framebuffer, PSO -------------------------------------------------
    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs = nullptr, ps = nullptr;
    smvi.codeSize = sizeof(VKFF_VERT_SPV); smvi.pCode = VKFF_VERT_SPV;
    bool ok = chk("CreateShaderModule(ff vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &vs));
    smvi.codeSize = sizeof(VKFF_PROBE_FRAG_SPV); smvi.pCode = VKFF_PROBE_FRAG_SPV;
    ok = ok && chk("CreateShaderModule(ff ps)", G.vkCreateShaderModule(dev, &smvi, nullptr, &ps));

    VkPipelineLayout playout = nullptr;
    if (ok) {
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &D4.setLayout;
        ok = chk("CreatePipelineLayout(ff probe)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &playout));
    }

    VkRenderPass opass = nullptr;
    if (ok) {
        VkAttachmentDescription att = {};
        att.format = fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref; sub.pDepthStencilAttachment = nullptr;
        VkSubpassDependency dp[2] = {};
        dp[0].srcSubpass = VK_SUBPASS_EXTERNAL; dp[0].dstSubpass = 0;
        dp[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dp[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dp[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dp[1].srcSubpass = 0; dp[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dp[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dp[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dp[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dp[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 1; rpi.pAttachments = &att;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        rpi.dependencyCount = 2; rpi.pDependencies = dp;
        ok = chk("CreateRenderPass(ff probe)", G.vkCreateRenderPass(dev, &rpi, nullptr, &opass));
    }

    VkFramebuffer fb = nullptr;
    if (ok) {
        VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbi.renderPass = opass; fbi.attachmentCount = 1; fbi.pAttachments = &col->view;
        fbi.width = FFPROBE_W; fbi.height = FFPROBE_H; fbi.layers = 1;
        ok = chk("CreateFramebuffer(ff probe)", G.vkCreateFramebuffer(dev, &fbi, nullptr, &fb));
    }

    VkPipeline pso = nullptr;
    if (ok) {
        VkVertexInputBindingDescription vbd = { 0, FFPROBE_VSTRIDE, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vad[8];
        for (int i = 0; i < 8; ++i) {
            vad[i] = {};
            vad[i].location = (uint32_t)i; vad[i].binding = 0;
            vad[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            vad[i].offset = (uint32_t)(i * 16);
        }
        VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbd;
        vin.vertexAttributeDescriptionCount = 8; vin.pVertexAttributeDescriptions = vad;
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp0 = { 0.0f, 0.0f, (float)FFPROBE_W, (float)FFPROBE_BH, 0.0f, 1.0f };
        VkRect2D sc0 = { { 0, 0 }, { FFPROBE_W, FFPROBE_BH } };
        VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vpi.viewportCount = 1; vpi.pViewports = &vp0; vpi.scissorCount = 1; vpi.pScissors = &sc0;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        // No depth attachment and no test: this probe measures colour, and wedge 4's own probe already
        // proves a depth attachment plus LESS compare keeps the later far draw off the near pixels.
        ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.blendEnable = VK_FALSE; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
        VkPipelineShaderStageCreateInfo stg[2] = {};
        stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = vs; stg[0].pName = "main";
        stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = ps; stg[1].pName = "main";
        VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount = 2; gpi.pStages = stg;
        gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
        gpi.layout = playout; gpi.renderPass = opass; gpi.subpass = 0;
        ok = chk("CreateGraphicsPipelines(ff probe)",
                 G.vkCreateGraphicsPipelines(dev, nullptr, 1, &gpi, nullptr, &pso));
    }

    VkDescriptorSet set = nullptr;
    if (ok) {
        rff.host = pFF[0]; rff.bytes = VKFF_STATE_BYTES; rff.offset = oFF[0];
        ok = vkdesc_alloc_set(&set);
        void* views[8] = { (void*)D4.whiteView, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        void* samps[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
        if (ok) ok = vkdesc_write_set(set, views, samps, &rtx, &rpx, &rff);
    }

    // ---- record, submit, read back ---------------------------------------------------------------
    const unsigned rowBytes = FFPROBE_W * texel;
    const unsigned long long stageBytes = (unsigned long long)rowBytes * FFPROBE_H + 16ull;
    if (ok && !vktex_ensure_stage(stageBytes)) ok = false;
    if (ok) {
        VkCommandBuffer cb2 = nullptr;
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        ok = chk("AllocCmd(ff probe)", G.vkAllocateCommandBuffers(dev, &cai, &cb2));
        if (ok) {
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            G.vkBeginCommandBuffer(cb2, &bbi);
            VkClearValue clr = {};
            // Float targets get a value no lighting equation can produce, so an undrawn pixel is
            // unmistakable; a UNORM target clamps it to 0 and the compare relies on the expected
            // channels being above that, which every band's rig is checked to guarantee.
            const float cv = byteFmt ? 0.0f : -1.0e18f;
            clr.color = { { cv, cv, cv, cv } };
            VkRenderPassBeginInfo rpb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpb.renderPass = opass; rpb.framebuffer = fb;
            rpb.renderArea = { { 0, 0 }, { FFPROBE_W, FFPROBE_H } };
            rpb.clearValueCount = 1; rpb.pClearValues = &clr;
            G.vkCmdBeginRenderPass(cb2, &rpb, VK_SUBPASS_CONTENTS_INLINE);
            G.vkCmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
            G.vkCmdBindVertexBuffers(cb2, 0, 1, &vbuf, &voff);
            for (int b = 0; b < FFPROBE_BANDS; ++b) {
                VkViewport vp = { 0.0f, (float)(b * FFPROBE_BH), (float)FFPROBE_W, (float)FFPROBE_BH, 0.0f, 1.0f };
                VkRect2D sc = { { 0, (int)(b * FFPROBE_BH) }, { FFPROBE_W, FFPROBE_BH } };
                G.vkCmdSetViewport(cb2, 0, 1, &vp);
                G.vkCmdSetScissor(cb2, 0, 1, &sc);
                uint32_t dOff[3] = { (uint32_t)rtx.offset, (uint32_t)rpx.offset, (uint32_t)oFF[b] };
                G.vkCmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &set, 3, dOff);
                ++D4.binds;
                G.vkCmdDraw(cb2, 6, 1, (uint32_t)(b * 6), 0);
            }
            G.vkCmdEndRenderPass(cb2);
            VkBufferImageCopy cpy = {};
            cpy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            cpy.imageExtent = { FFPROBE_W, FFPROBE_H, 1 };
            G.vkCmdCopyImageToBuffer(cb2, col->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TX.stage, 1, &cpy);
            ok = chk("EndCmd(ff probe)", G.vkEndCommandBuffer(cb2));
            if (ok) {
                VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                VkFence fence = nullptr;
                VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1; si.pCommandBuffers = &cb2;
                ok = chk("CreateFence(ff probe)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
                     chk("Submit(ff probe)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
                     chk("Wait(ff probe)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
                if (fence) G.vkDestroyFence(dev, fence, nullptr);
            }
            G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb2);
        }
        if (ok) { col->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ++RT.transitions; }
    }

    // ---- compare ------------------------------------------------------------------------------
    unsigned mismatches = 0, checked = 0, skipped = 0, rigsBad = 0;
    float maxAbsErr = 0.0f;
    if (ok) {
        const unsigned char* rb = (const unsigned char*)TX.stageMap;
        for (int b = 0; b < FFPROBE_BANDS; ++b) {
            const FFProbeBand& B = bands[b];
            float want[4];
            for (int ch = 0; ch < 4; ++ch) {
                // The byte rung stores the colour clamped to [0,1], so the expectation has to be clamped
                // the same way or a legitimately over-one lighting sum reads as a mismatch.
                want[ch] = byteFmt ? (B.expect[ch] < 0.0f ? 0.0f : (B.expect[ch] > 1.0f ? 1.0f : B.expect[ch]))
                                   : B.expect[ch];
            }
            unsigned bandMismatch = 0;
            int fmCol = -1, fmRow = -1;
            float fmGot[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            // The band's own row strip only, so a draw that landed in the wrong band is a mismatch here
            // rather than silence. A pixel centre inside the rect but closer to an edge than the margin
            // would be at the fill rule's mercy and is skipped - with these rectangles no centre is (all
            // edges fall on integers, all centres on half-integers), so skipped == 0 doubles as the
            // evidence that the compare never depended on a rasterisation detail.
            for (unsigned row = (unsigned)(b * FFPROBE_BH); row < (unsigned)((b + 1) * FFPROBE_BH); ++row) {
                const float py = (float)row + 0.5f;
                if (py <= B.rect[1] || py >= B.rect[3]) continue;
                for (unsigned x = 0; x < FFPROBE_W; ++x) {
                    const float px = (float)x + 0.5f;
                    if (px <= B.rect[0] || px >= B.rect[2]) continue;
                    const float dL = px - B.rect[0], dR = B.rect[2] - px;
                    const float dT = py - B.rect[1], dB = B.rect[3] - py;
                    const float near0 = dL < dR ? dL : dR, near1 = dT < dB ? dT : dB;
                    if ((near0 < near1 ? near0 : near1) < FFPROBE_EF_MARGIN) { ++skipped; continue; }
                    const unsigned char* texelP = rb + (size_t)row * rowBytes + (size_t)x * texel;
                    float got[4];
                    for (int ch = 0; ch < 4; ++ch) got[ch] = ff_channel(fmt, texelP, ch);
                    float worst = 0.0f;
                    for (int ch = 0; ch < 4; ++ch) {
                        const float d = got[ch] - want[ch];
                        const float a = d > 0.0f ? d : -d;
                        if (a > worst) worst = a;
                    }
                    ++checked;
                    if (worst > maxAbsErr) maxAbsErr = worst;
                    if (worst > eps) {
                        ++mismatches; ++bandMismatch;
                        if (fmCol < 0) {
                            fmCol = (int)x; fmRow = (int)row;
                            for (int ch = 0; ch < 4; ++ch) fmGot[ch] = got[ch];
                        }
                    }
                }
            }
            if (bandMismatch) ++rigsBad;
            if (bandMismatch) {
                VKWP1_LOG("VKFF probe band %d: %u mismatches, first at (%d,%d) got[%.5f %.5f %.5f %.5f] "
                          "want[%.5f %.5f %.5f %.5f] spread %.3e",
                    b, bandMismatch, fmCol, fmRow, fmGot[0], fmGot[1], fmGot[2], fmGot[3],
                    want[0], want[1], want[2], want[3], B.spread);
            } else {
                VKWP1_LOG("VKFF probe band %d ok: want[%.5f %.5f %.5f %.5f] spread %.3e",
                    b, want[0], want[1], want[2], want[3], B.spread);
            }
        }
    }

    if (ok && pso) G.vkDestroyPipeline(dev, pso, nullptr);
    if (set) G.vkFreeDescriptorSets(dev, D4.pool, 1, &set);
    if (fb) G.vkDestroyFramebuffer(dev, fb, nullptr);
    if (playout) G.vkDestroyPipelineLayout(dev, playout, nullptr);
    if (opass) G.vkDestroyRenderPass(dev, opass, nullptr);
    if (vs) G.vkDestroyShaderModule(dev, vs, nullptr);
    if (ps) G.vkDestroyShaderModule(dev, ps, nullptr);
    if (vmem) { G.vkUnmapMemory(dev, vmem); G.vkFreeMemory(dev, vmem, nullptr); G.vkDestroyBuffer(dev, vbuf, nullptr); }

    // ok is "the whole Vk sequence returned success"; pass is the acceptance bit the plan cares about:
    // every compared pixel within eps, no pixel left to a rasterisation edge, and every rig corner-constant.
    const int pass = (ok && mismatches == 0u && skipped == 0u) ? 1 : 0;
    VKWP1_LOG("VKFF probe: fmt=%s bands=%d checked=%u skipped=%u mismatches=%u rigBlocks=%u maxAbsErr=%.3e eps=%.3e ok=%d pass=%d",
        fmtName, FFPROBE_BANDS, checked, skipped, mismatches, rigsBad, maxAbsErr, eps, (int)ok, pass);
    if (!ok) VKWP1_LOG("VKFF probe FAILED before the compare - see the FAIL line above for which Vk call");
}

// ---------------------------------------------------------------------------------------------
//
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: the pixel-side acceptance probe.
//
// What it is for. vkff.frag is not a lighting model - it is a *table interpreter*: eight stages, 26
// combiner rows, seven argument selectors with two modifier bits each, a result-routing bit, an alpha
// test folded out of the state block, and a fog choice. A table that big is exactly the thing that
// cannot be reviewed by reading it, and every row of it has a documented number attached. So each row
// gets a band: one quad, drawn by the real vkff.vert + vkff.frag through a real pipeline into a real
// render-target mirror, with two real textures uploaded through the real wedge-3.2 mirror path and
// sampled by a real NEAREST sampler - then read back and compared against VKFF_Combine_Ref, the CPU
// transcription of the same documentation.
//
// What it proves and what it does not. It proves the GLSL's enums, arithmetic, argument routing and
// stage chaining agree with VulkanFF.cpp's, and that the state block reaches both stages intact. It
// does not prove the documentation is what DXVK did, because both sides were written from the
// documentation; the four places where the documentation itself is ambiguous (PREMODULATE, the LERP
// factor channel, D3DTA_CONSTANT, and fog's colour-only blend) are annotated at vkff.frag's header and
// belong to the visual A/B. It also does not prove filter quality: every band samples with NEAREST on
// purpose, so an expected texel is a texel and not a blend of four, which keeps the combiner the only
// variable. Sampler correctness is wedge 4's own probe's job.
//
// ---------------------------------------------------------------------------------------------

// Forty bands: the 26 D3DTOP values get one each (the four specular-mapping rows and the two bump rows
// included, since the bump rows are only "not carried" in the sense that their matrices are missing), the
// seven D3DTA_* selectors and their two modifier bits get theirs, and the rest cover the chaining cases
// the table cannot express in one stage: RESULTARG=TEMP, a disabled group inside an enabled stage, the
// second and third sampler slots, the set indirection, the projected divide, both fog families and the
// three alpha-test outcomes. That is 35 combiner bands, 2 fog bands and 3 alpha-test bands.
#define FFPIX_BANDS   40
#define FFPIX_W       FFPROBE_W
#define FFPIX_BH      FFPROBE_BH
#define FFPIX_H       (FFPIX_BANDS * FFPIX_BH)

// The clear colour of the pixel probe is magenta, not the vertex probe's out-of-range negative: this
// probe has bands whose whole expected answer IS "nothing was written", and a discard leaves the target
// exactly at the clear value. Magenta is the colour no combiner of these rigs can produce, and every
// band's own rig self-check below says so rather than trusting it.
static const float FFPIX_MAGENTA[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
#define FFPIX_MAGENTA_MIN_DIST 0.04f   // 10 of 255: how far a drawn band's colour must sit from magenta
// The floor on distinct expected colours, below which the probe refuses to draw. Bands legitimately share
// an answer when the table says two rows are the same function at these operands (the bump rows reduce to
// arg1, PREMODULATE evaluates as MODULATE), so this is a floor and not "all of them".
#define FFPIX_DISTINCT_MIN 20

// The D3DTOP / D3DTA / D3DCMP ordinals, spelled again for this translation unit the way the vertex
// probe spells the fog modes: exporting them through a header for one probe costs more than it saves.
enum {
    PX_TOP_DISABLE = 1,      PX_TOP_SELECTARG1,   PX_TOP_SELECTARG2,     PX_TOP_MODULATE,
    PX_TOP_MODULATE2X,       PX_TOP_MODULATE4X,   PX_TOP_ADD,            PX_TOP_ADDSIGNED,
    PX_TOP_ADDSIGNED2X,      PX_TOP_SUBTRACT,     PX_TOP_ADDSMOOTH,      PX_TOP_BLENDDIFFUSEALPHA,
    PX_TOP_BLENDTEXTUREALPHA, PX_TOP_BLENDFACTORALPHA, PX_TOP_BLENDTEXTUREALPHAPM,
    PX_TOP_BLENDCURRENTALPHA, PX_TOP_PREMODULATE, PX_TOP_MODULATEALPHA_ADDCOLOR,
    PX_TOP_MODULATECOLOR_ADDALPHA, PX_TOP_MODULATEINVALPHA_ADDCOLOR, PX_TOP_MODULATEINVCOLOR_ADDALPHA,
    PX_TOP_BUMPENVMAP,       PX_TOP_BUMPENVMAPLUMINANCE, PX_TOP_DOTPRODUCT3,
    PX_TOP_MULTIPLYADD,      PX_TOP_LERP                    // = 26
};
enum {
    PX_TA_DIFFUSE = 0, PX_TA_CURRENT = 1, PX_TA_TEXTURE = 2, PX_TA_TFACTOR = 3,
    PX_TA_SPECULAR = 4, PX_TA_TEMP = 5, PX_TA_CONSTANT = 6,
    PX_TA_COMPLEMENT = 0x10, PX_TA_ALPHAREPLICATE = 0x20
};
// 1-based like D3DCMPFUNC; VKFF_PACK_ALPHA_TEST converts to the shader's 0-based table.
enum {
    PX_CMP_NEVER = 1, PX_CMP_LESS, PX_CMP_EQUAL, PX_CMP_LESSEQUAL,
    PX_CMP_GREATER, PX_CMP_NOTEQUAL, PX_CMP_GREATEREQUAL, PX_CMP_ALWAYS
};
// D3DTTFF_* (the coordinate-transform flags one stage carries): the low bits count components, and the
// two PROJECTED bits say which one is the divisor.
#define PX_TTFF_COUNT2      2u
#define PX_TTFF_PROJECTED2  0x10u

// ---- the two textures -------------------------------------------------------------------------
// Slot 0 is 2x2 so an unprojected coordinate, a projected one and the set indirection can all be told
// apart, and so a flipped v axis shows up as a channel swap rather than as a pass. Slot 1 is 1x1 with
// deliberately small values, which keeps the SUBTRACT-style rows away from the clamp floor and ceiling
// where a wrong row would otherwise hide. Bytes are B,G,R,A because ww3d format 2 mirrors to
// VK_FORMAT_B8G8R8A8_UNORM, and the four entries are stored so that the texel at (column u, row v) with
// the v axis read the way the mirror stores it (row 0 = first four bytes) is:
//     (0,0) = B=102 G=51  R=204 A=153      (1,0) = B=204 G=153 R=51  A=102
//     (0,1) = B=51  G=204 R=153 A=255      (1,1) = B=153 G=51  R=102 A=51
static const unsigned char FFPIX_TEX0[4 * 4] = {
    102,  51, 204, 153,     204, 153,  51, 102,
     51, 204, 153, 255,     153,  51, 102,  51,
};
static const unsigned char FFPIX_TEX1[4] = { 25, 12, 20, 200 };
#define FFPIX_UV(u)  (((u) < 0.5f) ? 0 : 1)
#define FFPIX_VT(v)  (((v) < 0.5f) ? 0 : 1)

// Slot 0's texel as the CPU side believes the sampler returns it: the byte value divided by 255, in
// r,g,b,a order. Kept in one place because a wrong axis or a wrong channel order here is indistinguishable
// from a wrong combiner unless the texels are all different from each other - which is why they are.
static void ff_texel0_at(float u, float v, float* out_rgba)
{
    const int col = FFPIX_UV(u), row = FFPIX_VT(v);
    const unsigned char* p = FFPIX_TEX0 + (size_t)(row * 2 + col) * 4u;
    out_rgba[0] = (float)p[2] / 255.0f;
    out_rgba[1] = (float)p[1] / 255.0f;
    out_rgba[2] = (float)p[0] / 255.0f;
    out_rgba[3] = (float)p[3] / 255.0f;
}
static void ff_texel1_at(float* out_rgba)
{
    out_rgba[0] = (float)FFPIX_TEX1[2] / 255.0f;
    out_rgba[1] = (float)FFPIX_TEX1[1] / 255.0f;
    out_rgba[2] = (float)FFPIX_TEX1[0] / 255.0f;
    out_rgba[3] = (float)FFPIX_TEX1[3] / 255.0f;
}

// One stage's combiner registers, written the way the packer writes them: the four words of the colour
// group, then of the alpha group, then the result target. Two of the eight stage slots are enough for
// every band, and the rest stay DISABLE, which is the census-dominant configuration anyway.
static void ff_pix_stage(VKDraw_FF_State& ff, int s, unsigned cop, unsigned c0, unsigned c1, unsigned c2,
                         unsigned aop, unsigned a0, unsigned a1, unsigned a2, unsigned res)
{
    VKDraw_FF_TSS& T = ff.tss[s];
    T.col[0] = cop; T.col[1] = c0; T.col[2] = c1; T.col[3] = c2;
    T.alp[0] = aop; T.alp[1] = a0; T.alp[2] = a1; T.alp[3] = a2;
    T.res[0] = res;
}

// A band of the pixel probe: the rig (state block + six vertices + device rect, all of it built the way
// the vertex probe builds one, because the vertex stage is shared) plus whether the combiner reference
// says the band is alpha-tested away. The expected colour is the vertex probe's FFProbeBand::expect.
struct FFPixBand {
    FFProbeBand rig;
    int   discarded;              // the reference alpha-tested this band away, so the target must keep magenta
};

// One stage's registers, spelled short because forty bands write two hundred and forty of them.
#define PIXSTAGE(s, cop, c0, c1, c2, aop, a0, a1, a2, res) \
    ff_pix_stage(ff, (s), (cop), (c0), (c1), (c2), (aop), (a0), (a1), (a2), (res))

bool vkffpix_env_on()
{
    static bool checked = false;
    static bool on = false;
    if (!checked) {
        checked = true;
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKFFPIX", env, (DWORD)sizeof(env));
        on = (n != 0 && env[0] != '0');
    }
    return on;
}

// One band of the pixel probe. The rig is the vertex probe's rig with the lighting dialled off: the combiner
// starts from vColor, so what matters here is that vColor is a known constant (the vertex diffuse register,
// unlit) and that each corner's texture coordinate is *individually* placed - inside the same texel, but at
// four different points in it. That is what makes the compare catch a coordinate set fed to the wrong stage,
// a projected divide done in the vertex stage instead of the pixel stage, or a set-indirection applied twice,
// none of which a fully constant quad can see.
bool ff_pix_probe_band(int b, FFPixBand* out, FFProbeVert* verts)
{
    FFPixBand& P = *out;
    memset(&P, 0, sizeof(P));
    FFProbeBand& B = P.rig;
    VKDraw_FF_State& ff = B.ff;
    memset(&ff, 0, sizeof(ff));

    const float bandTop = (float)(b * FFPIX_BH);

    // ---- what a band is allowed to vary -----------------------------------------------------------
    // Everything else is the same for all forty: lighting off, so vColor IS the vertex diffuse register
    // and vSpecular is zero (the path the census says the majority of draws take, and the only one where a
    // combiner's answer has no lighting error riding on it); identity world, view and projection, so the
    // camera depth is the band's own plane z and the two fog families can be read off a number the CPU
    // reference already knows; one quad per band, at the same inset the vertex probe uses.
    struct Knobs {
        unsigned texcount;                 // the coordinate-set count the FVF carries
        float u[4], v[4], z[4], w[4];      // corner 0's four coordinate slots
        unsigned set[4], ttff[4];          // D3DTSS_TEXCOORDINDEX's set field, and the transform flags
        float tfac[4], vcol1[4];
        unsigned fogv, fogt; float fogS, fogE, fogD, fogC[4];
        int atEn, atFunc, atRef;
    } K;
    K.texcount = 1u;
    for (int t = 0; t < 4; ++t) {
        K.u[t] = 0.22f; K.v[t] = 0.20f; K.z[t] = 0.0f; K.w[t] = 1.0f;
        K.set[t] = (unsigned)t; K.ttff[t] = 0u;
    }
    K.u[1] = 0.72f; K.v[1] = 0.70f;      // slot 1 sits in a different texel from slot 0, so a set
    K.u[2] = 0.72f; K.v[2] = 0.20f;      // indirection that ignores the index changes the answer
    K.u[3] = 0.22f; K.v[3] = 0.70f;
    K.tfac[0] = K.tfac[1] = K.tfac[2] = K.tfac[3] = 0.0f;
    K.vcol1[0] = 0.20f; K.vcol1[1] = 0.40f; K.vcol1[2] = 0.80f; K.vcol1[3] = 0.60f;
    K.fogv = BB_FOG_NONE; K.fogt = BB_FOG_NONE;
    K.fogS = 0.0f; K.fogE = 0.0f; K.fogD = 0.0f;
    K.fogC[0] = 0.65f; K.fogC[1] = 0.15f; K.fogC[2] = 0.55f; K.fogC[3] = 0.0f;
    K.atEn = 0; K.atFunc = PX_CMP_ALWAYS; K.atRef = 0;

#define TFAC(x, y, z_, w_) K.tfac[0] = (x); K.tfac[1] = (y); K.tfac[2] = (z_); K.tfac[3] = (w_)
#define VCOL(x, y, z_, w_) K.vcol1[0] = (x); K.vcol1[1] = (y); K.vcol1[2] = (z_); K.vcol1[3] = (w_)
#define UV0(x, y)          K.u[0] = (x); K.v[0] = (y)

    // Every stage starts DISABLED in both channel groups. This is not the packer's default - the facade
    // installs the documented D3D9 defaults, MODULATE(TEXTURE,CURRENT), for a stage nobody wrote - it is the
    // rig's own default, chosen because a band must say which stages it uses. The distinction is not
    // cosmetic: D3DTOP has no ordinal 0, so a stage left at zero is neither DISABLE nor any row of the
    // table, and both sides answer it with the table's fall-through ("return arg1", arg1 defaulting to
    // DIFFUSE). Seven leftover zero stages therefore rewrite the combiner's running colour back to the
    // vertex colour, and because CPU reference and GLSL share that fall-through the probe prints a clean
    // pass while measuring nothing. An earlier run of this probe did exactly that.
    for (unsigned s = 0; s < (unsigned)VKFF_MAX_STAGES; ++s) {
        PIXSTAGE((int)s, PX_TOP_DISABLE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_DISABLE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
    }

    switch (b) {
    case 0:   // MODULATE, the census-dominant row: texel times vertex colour, both channel groups.
        PIXSTAGE(0, PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 1:   // Set indirection plus the second sampler slot: stage 0 reads coordinate set 1 and stage 1
              // reads set 0, so a stage that ignores D3DTSS_TEXCOORDINDEX gets the wrong texel.
        K.texcount = 2u; K.set[0] = 1u; K.set[1] = 0u;
        PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        PIXSTAGE(1, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 2:   // The mirror pair of row 0's selectors, with a third set allocated but unused - a stage
              // loop that walks the sets instead of the stages would sample set 2 here and change texel.
        K.texcount = 2u; UV0(0.72f, 0.20f);
        PIXSTAGE(0, PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 3:   // A disabled colour group inside an enabled stage keeps the current rgb while the alpha
              // group still runs, and the FVF carries no coordinate set at all: the vertex stage must
              // hand the sampler its documented (0,0,0,1) rather than whatever the attribute holds.
        K.texcount = 0u;
        PIXSTAGE(0, PX_TOP_DISABLE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE | PX_TA_ALPHAREPLICATE, PX_TA_CURRENT,
                 PX_TA_CURRENT);
        break;
    case 4:   PIXSTAGE(0, PX_TOP_ADDSIGNED, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_ADDSIGNED, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 5:   TFAC(0.10f, 0.20f, 0.30f, 0.30f);
        PIXSTAGE(0, PX_TOP_MODULATE4X, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR,
                 PX_TOP_MODULATE4X, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    case 6:   PIXSTAGE(0, PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE | PX_TA_COMPLEMENT,
                 PX_TA_CURRENT | PX_TA_COMPLEMENT,
                 PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE | PX_TA_COMPLEMENT,
                 PX_TA_CURRENT | PX_TA_COMPLEMENT, PX_TA_CURRENT);
        break;
    case 7:   PIXSTAGE(0, PX_TOP_PREMODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_PREMODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 8:   PIXSTAGE(0, PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE | PX_TA_ALPHAREPLICATE,
                 PX_TA_CURRENT | PX_TA_ALPHAREPLICATE,
                 PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE | PX_TA_ALPHAREPLICATE,
                 PX_TA_CURRENT | PX_TA_ALPHAREPLICATE, PX_TA_CURRENT);
        break;
    case 9:   TFAC(0.05f, 0.10f, 0.15f, 0.20f);
        PIXSTAGE(0, PX_TOP_ADD, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TFACTOR,
                 PX_TOP_ADD, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TFACTOR, PX_TA_CURRENT);
        PIXSTAGE(1, PX_TOP_SUBTRACT, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 10:  PIXSTAGE(0, PX_TOP_ADDSMOOTH, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_ADDSMOOTH, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 11:  PIXSTAGE(0, PX_TOP_BLENDCURRENTALPHA, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE, PX_TA_CURRENT);
        break;
    case 12:  TFAC(0.10f, 0.20f, 0.30f, 0.30f); VCOL(0.20f, 0.40f, 0.80f, 0.90f);
        PIXSTAGE(0, PX_TOP_BLENDDIFFUSEALPHA, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    case 13:  PIXSTAGE(0, PX_TOP_BLENDTEXTUREALPHA, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE, PX_TA_CURRENT);
        break;
    case 14:  PIXSTAGE(0, PX_TOP_BLENDTEXTUREALPHAPM, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE, PX_TA_CURRENT);
        break;
    case 15:  TFAC(0.0f, 0.0f, 0.0f, 0.55f);
        PIXSTAGE(0, PX_TOP_BLENDFACTORALPHA, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    case 16:  PIXSTAGE(0, PX_TOP_MODULATEALPHA_ADDCOLOR, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 17:  PIXSTAGE(0, PX_TOP_MODULATEINVALPHA_ADDCOLOR, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 18:  PIXSTAGE(0, PX_TOP_MODULATECOLOR_ADDALPHA, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 19:  TFAC(0.10f, 0.20f, 0.30f, 0.30f);
        PIXSTAGE(0, PX_TOP_MODULATEINVCOLOR_ADDALPHA, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    case 20:  TFAC(0.10f, 0.20f, 0.30f, 0.30f);
        PIXSTAGE(0, PX_TOP_MODULATE2X, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR,
                 PX_TOP_MODULATE2X, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    case 21:  TFAC(0.10f, 0.20f, 0.30f, 0.30f);
        PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TFACTOR, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 22:  PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_SPECULAR, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 23:  VCOL(0.35f, 0.55f, 0.75f, 0.65f);
        PIXSTAGE(0, PX_TOP_LERP, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_DIFFUSE | PX_TA_ALPHAREPLICATE,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 24:  PIXSTAGE(0, PX_TOP_LERP, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_DIFFUSE | PX_TA_COMPLEMENT,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_DIFFUSE | PX_TA_COMPLEMENT, PX_TA_CURRENT,
                 PX_TA_CURRENT);
        break;
    case 25:  TFAC(0.10f, 0.20f, 0.30f, 0.30f);
        PIXSTAGE(0, PX_TOP_LERP, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 26:  TFAC(0.10f, 0.20f, 0.30f, 0.30f);
        PIXSTAGE(0, PX_TOP_MULTIPLYADD, PX_TA_DIFFUSE, PX_TA_TFACTOR | PX_TA_ALPHAREPLICATE,
                 PX_TA_TEXTURE,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 27:  PIXSTAGE(0, PX_TOP_DOTPRODUCT3, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 28:  TFAC(0.70f, 0.60f, 0.90f, 0.30f);
        PIXSTAGE(0, PX_TOP_DOTPRODUCT3, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    case 29:  PIXSTAGE(0, PX_TOP_DOTPRODUCT3, PX_TA_DIFFUSE, PX_TA_TEXTURE,
                 PX_TA_TEXTURE | PX_TA_COMPLEMENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 30:  PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_CONSTANT, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 31:  PIXSTAGE(0, PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_TEMP);
        PIXSTAGE(1, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEMP, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 32:  PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        PIXSTAGE(1, PX_TOP_SUBTRACT, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        PIXSTAGE(2, PX_TOP_SUBTRACT, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE | PX_TA_COMPLEMENT,
                 PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_CURRENT, PX_TA_TEXTURE, PX_TA_CURRENT);
        break;
    case 33:  UV0(0.60f, 0.20f); K.z[0] = 2.0f; K.ttff[0] = PX_TTFF_COUNT2 | PX_TTFF_PROJECTED2;
        PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 34:  UV0(0.60f, 0.20f); K.z[0] = 2.0f; K.ttff[0] = PX_TTFF_COUNT2;
        PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 35:  K.fogt = BB_FOG_LINEAR; K.fogS = 0.1f; K.fogE = 0.5f;
        PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 36:  K.fogv = BB_FOG_LINEAR; K.fogS = 0.2f; K.fogE = 0.8f;
        PIXSTAGE(0, PX_TOP_SELECTARG1, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT, PX_TA_CURRENT);
        break;
    case 37:  TFAC(0.10f, 0.20f, 0.30f, 0.80f); K.atEn = 1; K.atFunc = PX_CMP_GREATER; K.atRef = 128;
        PIXSTAGE(0, PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    case 38:  TFAC(0.10f, 0.20f, 0.30f, 0.80f); K.atEn = 1; K.atFunc = PX_CMP_GREATER; K.atRef = 208;
        PIXSTAGE(0, PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    default:  TFAC(0.10f, 0.20f, 0.30f, 0.80f); K.atEn = 1; K.atFunc = PX_CMP_NEVER; K.atRef = 0;
        PIXSTAGE(0, PX_TOP_MODULATE, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_CURRENT,
                 PX_TOP_SELECTARG2, PX_TA_DIFFUSE, PX_TA_TEXTURE, PX_TA_TFACTOR, PX_TA_CURRENT);
        break;
    }
#undef TFAC
#undef VCOL
#undef UV0
#undef PIXSTAGE

    // ---- the block -------------------------------------------------------------------------------
    ff_id(ff.world); ff_id(ff.view); ff_id(ff.proj);
    for (unsigned s = 0; s < VKFF_MAX_STAGES; ++s) ff_id(ff.tex_mat[s]);   // BUG-03, see the vertex probe
    ff_set4(ff.mat_diffuse,  0.50f, 0.50f, 0.50f, 1.0f);   // never read: every band is unlit
    ff_set4(ff.mat_ambient,  0.50f, 0.50f, 0.50f, 1.0f);
    ff_set4(ff.mat_specular, 0.50f, 0.50f, 0.50f, 1.0f);
    ff_set4(ff.mat_emissive, 0.00f, 0.00f, 0.00f, 1.0f);
    ff_set4(ff.mat_power,    8.0f, 0.0f, 0.0f, 0.0f);
    ff_set4(ff.scene_ambient, 0.0f, 0.0f, 0.0f, 0.0f);
    ff_set4(ff.fog_color,     K.fogC[0], K.fogC[1], K.fogC[2], 0.0f);
    ff_set4(ff.fog_params,    K.fogS, K.fogE, K.fogD, 0.0f);
    ff_set4(ff.tex_factor,    K.tfac[0], K.tfac[1], K.tfac[2], K.tfac[3]);
    ff_set4(ff.viewport, 0.0f, bandTop, (float)FFPIX_W, (float)FFPIX_BH);
    ff_set4(ff.eye_position, 0.0f, 0.0f, 0.0f, 0.0f);

    unsigned fx = VKFF_FLAG_COLORVERTEX | VKFF_FLAG_HAS_DIFFUSE;
    if (K.fogv != BB_FOG_NONE || K.fogt != BB_FOG_NONE) fx |= VKFF_FLAG_FOG_ENABLE;
    ff.flags[0] = fx;
    ff.flags[1] = (K.texcount << 8) | (K.fogv << 11) | (K.fogt << 14) | (1u << 19);
    ff.flags[2] = VKFF_PACK_ALPHA_TEST(K.atEn, (unsigned)K.atFunc, (unsigned)K.atRef);
    ff.flags[3] = 0u;
    for (int s = 0; s < (int)VKFF_MAX_STAGES; ++s) {
        ff.stage[s].tci = 0u;                    // PASSTHRU: the camera-space generators are wedge 6's rigs
        ff.stage[s].components = 2u;
        ff.stage[s].set = (s < 4) ? K.set[s] : (unsigned)s;
        ff.stage[s].ttff = (s < 4) ? K.ttff[s] : 0u;
    }

    // ---- the geometry -----------------------------------------------------------------------------
    const float cx = 0.0f, cy = 0.0f, hw = 0.9375f, hh = 0.75f, zc = 0.3f;
    B.rect[0] = (cx - hw + 1.0f) * 0.5f * (float)FFPIX_W;
    B.rect[2] = (cx + hw + 1.0f) * 0.5f * (float)FFPIX_W;
    B.rect[1] = bandTop + (1.0f - (cy + hh)) * 0.5f * (float)FFPIX_BH;
    B.rect[3] = bandTop + (1.0f - (cy - hh)) * 0.5f * (float)FFPIX_BH;
    float wx[4], wy[4];
    wx[0] = cx - hw; wy[0] = cy + hh;
    wx[1] = cx - hw; wy[1] = cy - hh;
    wx[2] = cx + hw; wy[2] = cy + hh;
    wx[3] = cx + hw; wy[3] = cy - hh;
    const int order[6] = { 0, 1, 2, 1, 3, 2 };

    // Each corner's coordinate is displaced by six hundredths inside its own texel, which is what makes
    // this probe able to see a coordinate reaching the sampler at all: a stage that ignored the varying
    // and used a constant, or divided in the wrong stage, would move at least one corner across a texel
    // boundary. The displacement is deliberately smaller than the distance to the nearest boundary.
    const float du = 0.06f, dv = 0.06f;
    const float cuOff[4] = { 0.0f, 0.0f, du, du };
    const float cvOff[4] = { 0.0f, dv, 0.0f, dv };

    for (int k = 0; k < 6; ++k) {
        const int c = order[k];
        FFProbeVert& V = verts[k];
        memset(&V, 0, sizeof(V));
        V.pos[0] = wx[c]; V.pos[1] = wy[c]; V.pos[2] = zc; V.pos[3] = 1.0f;
        V.nrm[0] = 0.0f;  V.nrm[1] = 0.0f;  V.nrm[2] = 1.0f; V.nrm[3] = 0.0f;
        memcpy(V.dif, K.vcol1, sizeof(V.dif));
        V.spe[0] = 0.0f; V.spe[1] = 0.0f; V.spe[2] = 0.0f; V.spe[3] = 1.0f;
        for (int t = 0; t < 4; ++t) {
            V.tex[t][0] = K.u[t] + cuOff[c];
            V.tex[t][1] = K.v[t] + cvOff[c];
            V.tex[t][2] = K.z[t];
            V.tex[t][3] = K.w[t];
        }
    }

    // ---- the four corners through both references ----------------------------------------------------
    float want0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float spread = 0.0f;
    int discCorners = 0;
    for (int c = 0; c < 4; ++c) {
        VKFF_Vertex_In vin;
        VKFF_Vertex_Out vout;
        memset(&vin, 0, sizeof(vin));
        vin.world[0] = wx[c]; vin.world[1] = wy[c]; vin.world[2] = zc;
        vin.view[0] = wx[c];  vin.view[1] = wy[c];  vin.view[2] = zc;
        vin.normal[0] = 0.0f; vin.normal[1] = 0.0f; vin.normal[2] = 1.0f;
        memcpy(vin.diffuse, K.vcol1, sizeof(vin.diffuse));
        vin.specular[0] = 0.0f; vin.specular[1] = 0.0f;
        vin.specular[2] = 0.0f; vin.specular[3] = 1.0f;
        VKFF_Vertex_Ref(ff, vin, &vout);

        // The vertex stage's own coordinate resolve, replayed so the pixel reference is handed exactly the
        // four vec4s vkff.frag reads: the passthrough of the set the stage names, and the projected flag
        // moving .z into .w. The camera-space generators are wedge 6's rigs and are not re-derived here.
        float tc[4][4];
        for (int s = 0; s < 4; ++s) {
            float* o = tc[s];
            o[0] = 0.0f; o[1] = 0.0f; o[2] = 0.0f; o[3] = 1.0f;
            if ((unsigned)s < K.texcount) {
                const int k = (int)K.set[s] & 3;
                o[0] = K.u[k] + cuOff[c];
                o[1] = K.v[k] + cvOff[c];
                o[2] = K.z[k];
                o[3] = K.w[k];
                if ((K.ttff[s] & 0x7u) == 2u && (K.ttff[s] & PX_TTFF_PROJECTED2) != 0u) o[3] = o[2];
            }
        }

        VKFF_Pixel_In pin;
        memset(&pin, 0, sizeof(pin));
        memcpy(pin.color, vout.color, sizeof(pin.color));
        memcpy(pin.specular, vout.specular, sizeof(pin.specular));
        for (int s = 0; s < 4; ++s) {
            float* e = pin.texel[s];
            // Slots 2 and 3 are not bound in this probe, and the descriptor layer fills an unbound slot
            // with the built-in white - the same convention stage_texel() uses above the fourth stage.
            e[0] = 1.0f; e[1] = 1.0f; e[2] = 1.0f; e[3] = 1.0f;
            float su = tc[s][0], sv = tc[s][1];
            // The divide happens in the pixel stage, on the interpolated coordinate. Every band gives all
            // four corners the same w, so dividing before or after interpolation lands on the same point:
            // this rig proves the divide happens and with which coordinate, not where in the pipeline.
            if ((K.ttff[s] & 0x7u) == 2u && (K.ttff[s] & PX_TTFF_PROJECTED2) != 0u && fabsf(tc[s][3]) > 1e-8f) {
                su = tc[s][0] / tc[s][3];
                sv = tc[s][1] / tc[s][3];
            }
            if (s == 0) ff_texel0_at(su, sv, e);
            else if (s == 1) ff_texel1_at(e);
        }
        pin.fog_depth = zc;              // the view matrix is the identity, so camera depth is the plane's z
        pin.fog_vertex = vout.fog;

        float got[4];
        const int keep = VKFF_Combine_Ref(ff, pin, got);
        if (!keep) ++discCorners;
        for (int ch = 0; ch < 4; ++ch) {
            const float v = keep ? got[ch] : FFPIX_MAGENTA[ch];
            if (c == 0) want0[ch] = v;
            const float d = v - want0[ch];
            const float a = d > 0.0f ? d : -d;
            if (a > spread) spread = a;
        }
        ++B.corners;
        if (c == 3) for (int ch = 0; ch < 4; ++ch) B.expect[ch] = want0[ch];
    }
    P.discarded = (discCorners != 0) ? 1 : 0;
    if (discCorners != 0 && discCorners != 4) {
        VKWP1_LOG("VKFFPIX probe band %d: the alpha test kept some corners and discarded others (%d of 4)",
            b, 4 - discCorners);
        return false;
    }
    B.spread = spread;
    if (B.spread > FFPROBE_SPREAD_LIMIT) {
        VKWP1_LOG("VKFFPIX probe band %d: rig not corner-constant (spread %.3e) expect[%.5f %.5f %.5f %.5f]",
            b, B.spread, B.expect[0], B.expect[1], B.expect[2], B.expect[3]);
        return false;
    }

    // A band whose colour sits on top of the clear colour cannot be distinguished from a band that was
    // never drawn, which would turn a missing draw into a pass. The alpha-test bands are the deliberate
    // exception: their expected answer IS the clear colour.
    if (!P.discarded) {
        float md = 0.0f;
        for (int ch = 0; ch < 4; ++ch) {
            const float d = B.expect[ch] - FFPIX_MAGENTA[ch];
            const float a = d > 0.0f ? d : -d;
            if (a > md) md = a;
        }
        if (md < FFPIX_MAGENTA_MIN_DIST) {
            VKWP1_LOG("VKFFPIX probe band %d: rig too close to the clear colour (distance %.3f)", b, md);
            return false;
        }
    }

    // A channel pinned at 0 or 1 is where a saturation can hide a wrong row, so it is said out loud even
    // though the band still tests something. Which rows do this is a property of the table, not of the GPU.
    for (int ch = 0; ch < 4; ++ch) {
        if (P.discarded) break;
        if (B.expect[ch] <= 0.02f || B.expect[ch] >= 0.98f) {
            VKWP1_LOG("VKFFPIX probe band %d: channel %d sits at %.5f, within saturation reach",
                b, ch, B.expect[ch]);
            break;
        }
    }
    return true;
}

// The probe itself: forty rigs, one draw each, one readback. Everything here is the vertex probe's
// sequence with three additions - two sampled mirrors uploaded through wedge 3.2, a magenta clear that
// means "this band was alpha-tested away", and a per-band expectation that comes from the combiner
// reference rather than the lighting one.
void vkffpix_probe_locked()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (!D4.built) { VKWP1_LOG("VKFFPIX probe skipped: descriptor layer not built"); return; }
    if (!vkrt_env_on()) { VKWP1_LOG("VKFFPIX probe skipped: needs GENERALSX_VKRT for the RT mirror"); return; }
    if (!vktex_env_on()) { VKWP1_LOG("VKFFPIX probe skipped: needs GENERALSX_VKTEX for the sampled mirrors"); return; }

    VkDevice dev = g_vulkanCore->Device();

    // ---- the format ladder --------------------------------------------------------------------
    // The same three rungs as the vertex probe, for the same reason: full float is the only rung where a
    // wrong combiner row cannot hide behind 8-bit quantisation. Unlike the vertex probe, this one does not
    // need an out-of-range clear value to make an undrawn pixel unmistakable - magenta is representable in
    // all three rungs, and the rigs are self-checked to sit away from it.
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    float eps = 0.0f;
    const char* fmtName = "none";
    struct FFPixFmt { VkFormat f; float eps; const char* name; } ladder[3] = {
        { VK_FORMAT_R32G32B32A32_SFLOAT, 2e-3f,  "R32G32B32A32_SFLOAT" },
        { VK_FORMAT_R16G16B16A16_SFLOAT, 1e-2f,  "R16G16B16A16_SFLOAT" },
        { VK_FORMAT_B8G8R8A8_UNORM,      1.5f / 255.0f, "B8G8R8A8_UNORM" },
    };
    bool byteFmt = false;
    for (int i = 0; i < 3; ++i) {
        if (!ff_fmt_usable(ladder[i].f)) continue;
        fmt = ladder[i].f; eps = ladder[i].eps; fmtName = ladder[i].name;
        byteFmt = (i == 2);
        break;
    }
    if (fmt == VK_FORMAT_UNDEFINED) { VKWP1_LOG("VKFFPIX probe FAIL: no usable colour attachment format"); return; }
    const unsigned texel = ff_texel_bytes(fmt);

    // ---- the rigs -----------------------------------------------------------------------------
    static FFPixBand bands[FFPIX_BANDS];
    static FFProbeVert bandVerts[FFPIX_BANDS][6];
    bool rigOK = true;
    for (int b = 0; b < FFPIX_BANDS; ++b) {
        if (!ff_pix_probe_band(b, &bands[b], bandVerts[b])) rigOK = false;
    }
    if (!rigOK) {
        VKWP1_LOG("VKFFPIX probe FAILED before the draw: one or more rigs are not corner-constant "
                  "or cannot be told apart from the clear colour");
        return;
    }

    // ---- do the rigs actually separate the table? ----------------------------------------------
    // Corner-constancy and a magenta-distance check say each band is a valid measurement; they cannot say
    // the bands measure DIFFERENT things. Forty bands that all resolve to the same colour is exactly what
    // a shared degenerate branch looks like - both sides of a reference/implementation pair fail the same
    // way, mismatch count stays zero, and the probe reports a pass while testing nothing. So the number of
    // distinct expected colours among the drawn bands is measured, and a probe that collapses is refused
    // before it draws. Bands the alpha test discards are excluded: they all expect the clear colour by
    // design.
    {
        int drawn = 0, distinct = 0;
        for (int b = 0; b < FFPIX_BANDS; ++b) {
            if (bands[b].discarded) continue;
            ++drawn;
            bool seen = false;
            for (int p = 0; p < b && !seen; ++p) {
                if (bands[p].discarded) continue;
                seen = true;
                for (int ch = 0; ch < 4; ++ch) {
                    const float d = bands[p].rig.expect[ch] - bands[b].rig.expect[ch];
                    if ((d > 0.0f ? d : -d) > 1e-4f) { seen = false; break; }
                }
            }
            if (!seen) ++distinct;
        }
        if (distinct < FFPIX_DISTINCT_MIN) {
            VKWP1_LOG("VKFFPIX probe FAILED before the draw: %d drawn bands resolve to only %d distinct "
                      "colours (floor %d) - the rigs are not separating the table rows",
                drawn, distinct, FFPIX_DISTINCT_MIN);
            return;
        }
        VKWP1_LOG("VKFFPIX probe rigs: %d drawn bands, %d distinct expected colours (floor %d)",
            drawn, distinct, FFPIX_DISTINCT_MIN);
    }

    // ---- target, textures, vertex storage, constants -------------------------------------------
    if (!vkrt_create_locked(g_ffpixColorKey, FFPIX_W, FFPIX_H, fmt, false)) {
        VKWP1_LOG("VKFFPIX probe FAIL: rt mirror alloc (%s %ux%u)", fmtName, FFPIX_W, FFPIX_H);
        return;
    }
    VKRTImg* col = vkrt_find(g_ffpixColorKey);
    if (!col || !col->view) { VKWP1_LOG("VKFFPIX probe FAIL: rt view missing"); return; }

    // Two mirrors through the real wedge-3.2 upload path. Slot 0 is the 2x2 that makes a coordinate
    // reaching the sampler visible at all; slot 1 is the small 1x1 that keeps the subtract-style rows off
    // the clamp floor and ceiling. VKTex_Query is not usable here - it takes the queue lock this probe
    // already holds - so the entries are resolved with vktex_find().
    VKTex_Mip_Source src0[1];
    src0[0].bits = FFPIX_TEX0; src0[0].pitch = 8u;              // 2 texels of 4 bytes per row
    VKTex_Mip_Source src1[1];
    src1[0].bits = FFPIX_TEX1; src1[0].pitch = 4u;              // 1 texel
    if (!vktex_upload_2d_locked(2u, 2u, 1u, 2, src0, g_ffpixTexKey0) ||
        !vktex_upload_2d_locked(1u, 1u, 1u, 2, src1, g_ffpixTexKey1)) {
        VKWP1_LOG("VKFFPIX probe FAIL: texture mirror upload");
        return;
    }
    VKTexImg* tex0 = vktex_find(g_ffpixTexKey0);
    VKTexImg* tex1 = vktex_find(g_ffpixTexKey1);
    if (!tex0 || !tex1 || !tex0->view || !tex1->view) { VKWP1_LOG("VKFFPIX probe FAIL: texture views"); return; }

    const unsigned vbytes = FFPIX_BANDS * 6u * FFPROBE_VSTRIDE;
    VkBuffer vbuf = nullptr; VkDeviceMemory vmem = nullptr; void* vmap = nullptr; VkDeviceSize voff = 0;
    if (g_vulkanCore->GeoRingUp()) {
        vmap = g_vulkanCore->Geo_Alloc(vbytes, false);
        if (vmap) {
            vbuf = (VkBuffer)g_vulkanCore->GeoBufferHandle();
            voff = (VkDeviceSize)g_vulkanCore->GeoOffsetOf(vmap);
        }
    }
    if (!vmap) { // ring unavailable or full: a dedicated host buffer keeps the probe meaningful
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = vbytes; bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!chk("CreateBuffer(ffpix vb)", G.vkCreateBuffer(dev, &bci, nullptr, &vbuf))) return;
        VkMemoryRequirements req; G.vkGetBufferMemoryRequirements(dev, vbuf, &req);
        uint32_t mt = vktex_memtype(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
        if (mt == ~0u ||
            !chk("AllocMemory(ffpix vb)", G.vkAllocateMemory(dev, &mai, nullptr, &vmem)) ||
            !chk("BindMemory(ffpix vb)", G.vkBindBufferMemory(dev, vbuf, vmem, 0)) ||
            !chk("MapMemory(ffpix vb)", G.vkMapMemory(dev, vmem, 0, vbytes, 0, &vmap))) {
            if (vmem) { G.vkUnmapMemory(dev, vmem); G.vkFreeMemory(dev, vmem, nullptr); }
            if (vbuf) { G.vkDestroyBuffer(dev, vbuf, nullptr); }
            return;
        }
        voff = 0;
    }
    if (!vmap) { VKWP1_LOG("VKFFPIX probe FAIL: no vertex storage"); return; }
    // Geo_Alloc hands back the mapped pointer of the reservation; voff is the same position as a device
    // offset for vkCmdBindVertexBuffers. The host write base is vmap alone (wedge 6's lesson).
    unsigned char* vbase = (unsigned char*)vmap;
    for (int b = 0; b < FFPIX_BANDS; ++b)
        memcpy(vbase + (size_t)b * 6u * FFPROBE_VSTRIDE, bandVerts[b], 6u * FFPROBE_VSTRIDE);

    const unsigned VTX_BYTES = 256u * 16u, PIX_BYTES = 224u * 16u;
    VKDesc_Region rtx, rpx, rff;
    unsigned long long oVtx = 0, oPix = 0, oFF[FFPIX_BANDS] = { 0 };
    void* pVtx = vkdesc_reserve_locked(VKDESC_RING_VERTEX, VTX_BYTES, &oVtx);
    void* pPix = vkdesc_reserve_locked(VKDESC_RING_PIXEL, PIX_BYTES, &oPix);
    void* pFF[FFPIX_BANDS] = { nullptr };
    for (int b = 0; b < FFPIX_BANDS; ++b) pFF[b] = vkdesc_reserve_locked(VKDESC_RING_LIGHTS, VKFF_STATE_BYTES, &oFF[b]);
    if (!pVtx || !pPix) { VKWP1_LOG("VKFFPIX probe FAIL: vtx/pix constant reservations"); return; }
    for (int b = 0; b < FFPIX_BANDS; ++b) {
        if (!pFF[b]) {
            VKWP1_LOG("VKFFPIX probe FAIL: FF ring reservation for band %d (lights ring peak %llu)",
                b, D4.ring[VKDESC_RING_LIGHTS].peak);
            return;
        }
        memcpy(pFF[b], &bands[b].rig.ff, VKFF_STATE_BYTES);
    }
    memset(pVtx, 0, VTX_BYTES);
    memset(pPix, 0, PIX_BYTES);
    rtx.host = pVtx; rtx.bytes = VTX_BYTES; rtx.offset = oVtx;
    rpx.host = pPix; rpx.bytes = PIX_BYTES; rpx.offset = oPix;

    // ---- modules, layout, pass, framebuffer, PSO ------------------------------------------------
    // VKFF_FRAG_SPV, not the vertex probe's VKFF_PROBE_FRAG_SPV: the thing under test IS vkff.frag, so
    // this probe cannot borrow a fragment stage that passes the combiner's work off as a constant.
    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs = nullptr, ps = nullptr;
    smvi.codeSize = sizeof(VKFF_VERT_SPV); smvi.pCode = VKFF_VERT_SPV;
    bool ok = chk("CreateShaderModule(ffpix vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &vs));
    smvi.codeSize = sizeof(VKFF_FRAG_SPV); smvi.pCode = VKFF_FRAG_SPV;
    ok = ok && chk("CreateShaderModule(ffpix ps)", G.vkCreateShaderModule(dev, &smvi, nullptr, &ps));

    VkPipelineLayout playout = nullptr;
    if (ok) {
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &D4.setLayout;
        ok = chk("CreatePipelineLayout(ffpix probe)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &playout));
    }

    VkRenderPass opass = nullptr;
    if (ok) {
        VkAttachmentDescription att = {};
        att.format = fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref; sub.pDepthStencilAttachment = nullptr;
        VkSubpassDependency dp[2] = {};
        dp[0].srcSubpass = VK_SUBPASS_EXTERNAL; dp[0].dstSubpass = 0;
        dp[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dp[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dp[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dp[1].srcSubpass = 0; dp[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dp[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dp[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dp[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dp[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpi.attachmentCount = 1; rpi.pAttachments = &att;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        rpi.dependencyCount = 2; rpi.pDependencies = dp;
        ok = chk("CreateRenderPass(ffpix probe)", G.vkCreateRenderPass(dev, &rpi, nullptr, &opass));
    }

    VkFramebuffer fb = nullptr;
    if (ok) {
        VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbi.renderPass = opass; fbi.attachmentCount = 1; fbi.pAttachments = &col->view;
        fbi.width = FFPIX_W; fbi.height = FFPIX_H; fbi.layers = 1;
        ok = chk("CreateFramebuffer(ffpix probe)", G.vkCreateFramebuffer(dev, &fbi, nullptr, &fb));
    }

    VkPipeline pso = nullptr;
    if (ok) {
        VkVertexInputBindingDescription vbd = { 0, FFPROBE_VSTRIDE, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vad[8];
        for (int i = 0; i < 8; ++i) {
            vad[i] = {};
            vad[i].location = (uint32_t)i; vad[i].binding = 0;
            vad[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            vad[i].offset = (uint32_t)(i * 16);
        }
        VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbd;
        vin.vertexAttributeDescriptionCount = 8; vin.pVertexAttributeDescriptions = vad;
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp0 = { 0.0f, 0.0f, (float)FFPIX_W, (float)FFPIX_BH, 0.0f, 1.0f };
        VkRect2D sc0 = { { 0, 0 }, { FFPIX_W, FFPIX_BH } };
        VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vpi.viewportCount = 1; vpi.pViewports = &vp0; vpi.scissorCount = 1; vpi.pScissors = &sc0;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = VK_FALSE; ds.depthWriteEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.blendEnable = VK_FALSE; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
        VkPipelineShaderStageCreateInfo stg[2] = {};
        stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = vs; stg[0].pName = "main";
        stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = ps; stg[1].pName = "main";
        VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount = 2; gpi.pStages = stg;
        gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
        gpi.layout = playout; gpi.renderPass = opass; gpi.subpass = 0;
        ok = chk("CreateGraphicsPipelines(ffpix probe)",
                 G.vkCreateGraphicsPipelines(dev, nullptr, 1, &gpi, nullptr, &pso));
    }

    // One descriptor set for forty draws: binding 3 is dynamic, so only its offset moves between bands,
    // which is also what proves the packer's per-band block reaches the shader. Every slot samples with
    // point filters and WRAP so an expected texel is a texel, not a blend of four - the combiner is meant
    // to be the only variable in this probe.
    VkDescriptorSet set = nullptr;
    if (ok) {
        rff.host = pFF[0]; rff.bytes = VKFF_STATE_BYTES; rff.offset = oFF[0];
        void* sNearest = vkdesc_sampler_locked(1u, 1u, 1u, 1u, 1u, 1u, 1u);
        if (!sNearest) sNearest = (void*)D4.defSampler;
        ok = vkdesc_alloc_set(&set);
        void* views[8] = { (void*)tex0->view, (void*)tex1->view, (void*)D4.whiteView, (void*)D4.whiteView,
                           nullptr, nullptr, nullptr, nullptr };
        void* samps[8] = { sNearest, sNearest, sNearest, sNearest, nullptr, nullptr, nullptr, nullptr };
        if (ok) ok = vkdesc_write_set(set, views, samps, &rtx, &rpx, &rff);
    }

    // ---- record, submit, read back ---------------------------------------------------------------
    const unsigned rowBytes = FFPIX_W * texel;
    const unsigned long long stageBytes = (unsigned long long)rowBytes * FFPIX_H + 16ull;
    if (ok && !vktex_ensure_stage(stageBytes)) ok = false;
    if (ok) {
        VkCommandBuffer cb2 = nullptr;
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_vulkanCore->Pool(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        ok = chk("AllocCmd(ffpix probe)", G.vkAllocateCommandBuffers(dev, &cai, &cb2));
        if (ok) {
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            G.vkBeginCommandBuffer(cb2, &bbi);
            VkClearValue clr = {};
            clr.color = { { FFPIX_MAGENTA[0], FFPIX_MAGENTA[1], FFPIX_MAGENTA[2], FFPIX_MAGENTA[3] } };
            VkRenderPassBeginInfo rpb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rpb.renderPass = opass; rpb.framebuffer = fb;
            rpb.renderArea = { { 0, 0 }, { FFPIX_W, FFPIX_H } };
            rpb.clearValueCount = 1; rpb.pClearValues = &clr;
            G.vkCmdBeginRenderPass(cb2, &rpb, VK_SUBPASS_CONTENTS_INLINE);
            G.vkCmdBindPipeline(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
            G.vkCmdBindVertexBuffers(cb2, 0, 1, &vbuf, &voff);
            for (int b = 0; b < FFPIX_BANDS; ++b) {
                VkViewport vp = { 0.0f, (float)(b * FFPIX_BH), (float)FFPIX_W, (float)FFPIX_BH, 0.0f, 1.0f };
                VkRect2D sc = { { 0, (int)(b * FFPIX_BH) }, { FFPIX_W, FFPIX_BH } };
                G.vkCmdSetViewport(cb2, 0, 1, &vp);
                G.vkCmdSetScissor(cb2, 0, 1, &sc);
                uint32_t dOff[3] = { (uint32_t)rtx.offset, (uint32_t)rpx.offset, (uint32_t)oFF[b] };
                G.vkCmdBindDescriptorSets(cb2, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &set, 3, dOff);
                ++D4.binds;
                G.vkCmdDraw(cb2, 6, 1, (uint32_t)(b * 6), 0);
            }
            G.vkCmdEndRenderPass(cb2);
            VkBufferImageCopy cpy = {};
            cpy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            cpy.imageExtent = { FFPIX_W, FFPIX_H, 1 };
            G.vkCmdCopyImageToBuffer(cb2, col->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, TX.stage, 1, &cpy);
            ok = chk("EndCmd(ffpix probe)", G.vkEndCommandBuffer(cb2));
            if (ok) {
                VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                VkFence fence = nullptr;
                VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1; si.pCommandBuffers = &cb2;
                ok = chk("CreateFence(ffpix probe)", G.vkCreateFence(dev, &fi, nullptr, &fence)) &&
                     chk("Submit(ffpix probe)", G.vkQueueSubmit(g_vulkanCore->Queue(), 1, &si, fence)) &&
                     chk("Wait(ffpix probe)", G.vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
                if (fence) G.vkDestroyFence(dev, fence, nullptr);
            }
            G.vkFreeCommandBuffers(dev, g_vulkanCore->Pool(), 1, &cb2);
        }
        if (ok) { col->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; ++RT.transitions; }
    }

    // ---- compare ------------------------------------------------------------------------------
    unsigned mismatches = 0, checked = 0, skipped = 0, rigsBad = 0, discardedBands = 0;
    float maxAbsErr = 0.0f;
    if (ok) {
        const unsigned char* rb = (const unsigned char*)TX.stageMap;
        for (int b = 0; b < FFPIX_BANDS; ++b) {
            const FFProbeBand& B = bands[b].rig;
            const bool discarded = bands[b].discarded != 0;
            if (discarded) ++discardedBands;
            float want[4];
            for (int ch = 0; ch < 4; ++ch) {
                // A discarded band expects the clear value; a drawn one expects the combiner's answer,
                // clamped the way a UNORM target clamps it so a legitimately over-one sum is not a mismatch.
                float w = discarded ? FFPIX_MAGENTA[ch] : B.expect[ch];
                want[ch] = byteFmt ? (w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w)) : w;
            }
            unsigned bandMismatch = 0;
            int fmCol = -1, fmRow = -1;
            float fmGot[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (unsigned row = (unsigned)(b * FFPIX_BH); row < (unsigned)((b + 1) * FFPIX_BH); ++row) {
                const float py = (float)row + 0.5f;
                if (py <= B.rect[1] || py >= B.rect[3]) continue;
                for (unsigned x = 0; x < FFPIX_W; ++x) {
                    const float px = (float)x + 0.5f;
                    if (px <= B.rect[0] || px >= B.rect[2]) continue;
                    const float dL = px - B.rect[0], dR = B.rect[2] - px;
                    const float dT = py - B.rect[1], dB = B.rect[3] - py;
                    const float near0 = dL < dR ? dL : dR, near1 = dT < dB ? dT : dB;
                    if ((near0 < near1 ? near0 : near1) < FFPROBE_EF_MARGIN) { ++skipped; continue; }
                    const unsigned char* texelP = rb + (size_t)row * rowBytes + (size_t)x * texel;
                    float got[4];
                    for (int ch = 0; ch < 4; ++ch) got[ch] = ff_channel(fmt, texelP, ch);
                    float worst = 0.0f;
                    for (int ch = 0; ch < 4; ++ch) {
                        const float d = got[ch] - want[ch];
                        const float a = d > 0.0f ? d : -d;
                        if (a > worst) worst = a;
                    }
                    ++checked;
                    if (worst > maxAbsErr) maxAbsErr = worst;
                    if (worst > eps) {
                        ++mismatches; ++bandMismatch;
                        if (fmCol < 0) {
                            fmCol = (int)x; fmRow = (int)row;
                            for (int ch = 0; ch < 4; ++ch) fmGot[ch] = got[ch];
                        }
                    }
                }
            }
            if (bandMismatch) {
                ++rigsBad;
                VKWP1_LOG("VKFFPIX probe band %d: %u mismatches%s, first at (%d,%d) got[%.5f %.5f %.5f %.5f] "
                          "want[%.5f %.5f %.5f %.5f] spread %.3e",
                    b, bandMismatch, discarded ? " (alpha-tested band kept something)" : "",
                    fmCol, fmRow, fmGot[0], fmGot[1], fmGot[2], fmGot[3],
                    want[0], want[1], want[2], want[3], B.spread);
            } else {
                VKWP1_LOG("VKFFPIX probe band %d ok%s: want[%.5f %.5f %.5f %.5f] spread %.3e",
                    b, discarded ? " (discarded)" : "", want[0], want[1], want[2], want[3], B.spread);
            }
        }
    }

    if (ok && pso) G.vkDestroyPipeline(dev, pso, nullptr);
    if (set) G.vkFreeDescriptorSets(dev, D4.pool, 1, &set);
    if (fb) G.vkDestroyFramebuffer(dev, fb, nullptr);
    if (playout) G.vkDestroyPipelineLayout(dev, playout, nullptr);
    if (opass) G.vkDestroyRenderPass(dev, opass, nullptr);
    if (vs) G.vkDestroyShaderModule(dev, vs, nullptr);
    if (ps) G.vkDestroyShaderModule(dev, ps, nullptr);
    if (vmem) { G.vkUnmapMemory(dev, vmem); G.vkFreeMemory(dev, vmem, nullptr); G.vkDestroyBuffer(dev, vbuf, nullptr); }

    const int pass = (ok && mismatches == 0u && skipped == 0u) ? 1 : 0;
    VKWP1_LOG("VKFFPIX probe: fmt=%s bands=%d checked=%u skipped=%u mismatches=%u discardedBands=%u "
              "rigFailures=%u maxAbsErr=%.3e eps=%.3e ok=%d pass=%d",
        fmtName, FFPIX_BANDS, checked, skipped, mismatches, discardedBands, rigsBad,
        maxAbsErr, eps, (int)ok, pass);
    if (!ok) VKWP1_LOG("VKFFPIX probe FAILED before the compare - see the FAIL line above for which Vk call");
}

} // anon

// ---- public wedge 4 wrappers (all serialize on the shared queue lock) ----
bool VKDesc_Ready()
{
    return g_vulkanCore && g_vulkanCore->IsReady() && vkdesc_env_on() && D4.built;
}

bool VKDesc_Create()
{
    if (!g_vulkanCore || !g_vulkanCore->IsReady() || !vkdesc_env_on()) return false;
    vk_queue_lock();
    bool r = vkdesc_build();
    vk_queue_unlock();
    return r;
}

void VKDesc_Destroy()
{
    if (!g_vulkanCore) return;
    VkDevice dev = g_vulkanCore->Device();
    vk_queue_lock();
    if (dev) {
        for (auto& s : D4.sampVals) if (s) G.vkDestroySampler(dev, s, nullptr);
        D4.sampVals.clear(); D4.sampKeys.clear();
        if (D4.defSampler) G.vkDestroySampler(dev, D4.defSampler, nullptr);
        D4.defSampler = nullptr;
        if (D4.whiteView) G.vkDestroyImageView(dev, D4.whiteView, nullptr);
        if (D4.whiteImg) G.vkDestroyImage(dev, D4.whiteImg, nullptr);
        if (D4.whiteMem) G.vkFreeMemory(dev, D4.whiteMem, nullptr);
        D4.whiteView = nullptr; D4.whiteImg = nullptr; D4.whiteMem = nullptr;
        for (int i = 0; i < VKDESC_RING_COUNT; ++i) {
            VKDescRing& r = D4.ring[i];
            if (r.map) G.vkUnmapMemory(dev, r.mem);
            if (r.mem) G.vkFreeMemory(dev, r.mem, nullptr);
            if (r.buf) G.vkDestroyBuffer(dev, r.buf, nullptr);
            r.map = nullptr; r.mem = nullptr; r.buf = nullptr; r.cap = 0;
        }
        if (D4.pool) G.vkDestroyDescriptorPool(dev, D4.pool, nullptr);
        if (D4.setLayout) G.vkDestroyDescriptorSetLayout(dev, D4.setLayout, nullptr);
    }
    D4.pool = nullptr; D4.setLayout = nullptr; D4.built = false;
    vk_queue_unlock();
}

bool VKDesc_Uniform_Reserve(int ring, unsigned int size, VKDesc_Region* out)
{
    if (!out) return false;
    out->host = nullptr; out->offset = 0; out->bytes = 0;
    if (!VKDesc_Ready()) return false;
    vk_queue_lock();
    unsigned long long off = 0;
    void* p = vkdesc_reserve_locked(ring, size, &off);
    vk_queue_unlock();
    if (!p) return false;
    out->host = p; out->offset = off; out->bytes = size;
    return true;
}

void* VKDesc_Ring_Buffer(int ring)
{
    if (!D4.built || ring < 0 || ring >= VKDESC_RING_COUNT) return nullptr;
    return (void*)D4.ring[ring].buf;
}

void* VKDesc_Sampler(unsigned int addrU, unsigned int addrV, unsigned int addrW,
                     unsigned int minFilter, unsigned int magFilter, unsigned int mipFilter,
                     unsigned int maxAnisotropy)
{
    if (!VKDesc_Ready()) return nullptr;
    vk_queue_lock();
    void* s = vkdesc_sampler_locked(addrU, addrV, addrW, minFilter, magFilter, mipFilter, maxAnisotropy);
    vk_queue_unlock();
    return s;
}

bool VKDesc_Build_Set(void** set, void* const views[8], void* const samplers[8],
                      const VKDesc_Region* vtx, const VKDesc_Region* pix, const VKDesc_Region* lights)
{
    if (!VKDesc_Ready() || !set) return false;
    vk_queue_lock();
    VkDescriptorSet s = nullptr;
    bool ok = vkdesc_alloc_set(&s) && vkdesc_write_set(s, views, samplers, vtx, pix, lights);
    vk_queue_unlock();
    *set = ok ? (void*)s : nullptr;
    return ok;
}

void VKDesc_Run_Probe()
{
    if (!vkdesc_env_on() || !g_vulkanCore || !g_vulkanCore->IsReady()) return;
    vk_queue_lock();
    if (vkdesc_build()) vkdesc_probe_locked();
    vk_queue_unlock();
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function vertex stage's acceptance
// probe. It needs wedge 4 (the set layout, the constant rings and the pool) and wedge 3.4 (a keyed RT
// mirror to draw into), so it is its own gate rather than a side effect of either.
void VKFF_Run_Probe()
{
    if (!vkff_env_on() || !g_vulkanCore || !g_vulkanCore->IsReady()) return;
    if (!vkdesc_env_on()) {
        VKWP1_LOG("VKFF probe skipped: needs GENERALSX_VKDESC for the descriptor and constant layer");
        return;
    }
    vk_queue_lock();
    if (vkdesc_build()) vkff_probe_locked();
    vk_queue_unlock();
}

// GeneralsX @feature Qoder 18/10/2026 wedge 7's gate. It sits behind its own environment variable rather
// than running with the vertex probe because the two have different costs (forty draws against ten) and
// different failure meanings: a vertex-probe failure says the state block or the lighting equations are
// wrong, a pixel-probe failure says a combiner row is. Both need the vertex stage alive, so vkff_env_on()
// is a precondition rather than a convenience.
void VKFF_Run_Pixel_Probe()
{
    if (!vkffpix_env_on() || !vkff_env_on() || !g_vulkanCore || !g_vulkanCore->IsReady()) return;
    if (!vkdesc_env_on()) {
        VKWP1_LOG("VKFFPIX probe skipped: needs GENERALSX_VKDESC for the descriptor and constant layer");
        return;
    }
    vk_queue_lock();
    if (vkdesc_build()) vkffpix_probe_locked();
    vk_queue_unlock();
}

// ---- wedge 7 forensics: does a real frame contain a picture -----------------------------------
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7.
//
// What it answers. The probes for wedges 4-7 all say the same thing: on a rig this layer built, the GPU
// agrees with the CPU reference. That is arithmetic, not evidence about the game - a run can have every
// probe green and still show nothing, because "the pixel interpreter computes wrong" and "the draw path
// never took over" both arrive as a flat screen. So this samples the scene target itself a few times a
// second and prints what it found: mean colour, how much of it is not near-black, and how many distinct
// colour bins it covers. A clear field reports bins=1, the mechanism stage reports one flat colour, an
// actual frame reports a spread.
//
// Cost and safety. One 96 KiB host-visible buffer, six tightly-sized copy commands every interval, and
// the read happens at a frame Begin that has just waited a fence - never on this sample's own frame, but
// on the Begin that reuses the slot it was recorded into, which is the frame whose GPU work is provably
// finished. The copy is recorded right after the present blit, which already left the scene image in
// TRANSFER_SRC_OPTIMAL, so the sampler adds no barrier and cannot disturb the layout state machine.
namespace {

#define VKLUMA_PATCH   64u                    // sample edge, in pixels
#define VKLUMA_COLS     3u                    // a 3x2 grid across the scene, so a lit HUD corner or a
#define VKLUMA_ROWS     2u                    // dark middle cannot hide behind a single centre patch
#define VKLUMA_PATCHES (VKLUMA_COLS * VKLUMA_ROWS)
#define VKLUMA_SLOT  (VKLUMA_PATCH * VKLUMA_PATCH * 4ull)   // 16 KiB per patch, tight rows
#define VKLUMA_BYTES (VKLUMA_SLOT * VKLUMA_PATCHES)

struct VKLuma {
    bool  built;
    bool  pending;                  // a sample was recorded and has not been read yet
    unsigned long long frames;      // frames offered to the sampler
    unsigned long long reports;
    unsigned long long interval;
    VkBuffer buf; VkDeviceMemory mem; unsigned char* map;
    unsigned w, h;                  // scene extent the pending sample came from
    unsigned pw, ph;                // patch size actually copied (the scene can be smaller than one)
    unsigned recSlot;               // frame slot the pending sample was recorded into
    unsigned stuck;                 // record attempts refused because a sample was never read
    unsigned cx[VKLUMA_PATCHES], cy[VKLUMA_PATCHES];   // top-left of each patch, for the report
};
VKLuma LM = { false, false, 0ull, 0ull, 30ull, nullptr, nullptr, nullptr, 0u, 0u, 0u, 0u, 0u, 0u,
              { 0,0,0,0,0,0 }, { 0,0,0,0,0,0 } };

bool vkluma_build_locked()
{
    if (LM.built) return true;
    if (!vk_env_gate("GENERALSX_VKLUMA")) return false;
    VkDevice dev = g_vulkanCore->Device();
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = VKLUMA_BYTES;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("VKLUMA CreateBuffer", G.vkCreateBuffer(dev, &bi, nullptr, &LM.buf))) return false;
    VkMemoryRequirements req = {};
    G.vkGetBufferMemoryRequirements(dev, LM.buf, &req);
    const uint32_t mt = vktex_memtype(req,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    bool ok = mt != ~0u &&
              chk("VKLUMA AllocMemory", G.vkAllocateMemory(dev, &mai, nullptr, &LM.mem)) &&
              chk("VKLUMA BindBuffer", G.vkBindBufferMemory(dev, LM.buf, LM.mem, 0)) &&
              chk("VKLUMA Map", G.vkMapMemory(dev, LM.mem, 0, VKLUMA_BYTES, 0, (void**)&LM.map));
    if (!ok) {
        if (LM.map) { G.vkUnmapMemory(dev, LM.mem); LM.map = nullptr; }
        if (LM.mem) { G.vkFreeMemory(dev, LM.mem, nullptr); LM.mem = nullptr; }
        if (LM.buf) { G.vkDestroyBuffer(dev, LM.buf, nullptr); LM.buf = nullptr; }
        return false;
    }
    char every[16] = { 0 };
    if (GetEnvironmentVariableA("GENERALSX_VKLUMA_EVERY", every, (DWORD)sizeof(every)) > 0) {
        const long n = atol(every);
        if (n >= 1) LM.interval = (unsigned long long)n;
    }
    LM.built = true;
    VKWP1_LOG("VKLUMA sampling on: %ux%u patches in a %ux%u grid every %llu frames (first frame included)",
        (unsigned)VKLUMA_PATCH, (unsigned)VKLUMA_PATCH, (unsigned)VKLUMA_COLS, (unsigned)VKLUMA_ROWS,
        LM.interval);
    return true;
}

} // anon

void VKCore_Luma_Record(void* cmd_, void* sceneImage_, unsigned w, unsigned h, unsigned slot)
{
    if (!g_vulkanCore || !g_vulkanCore->IsReady()) return;
    vk_queue_lock();
    if (!vkluma_build_locked()) { vk_queue_unlock(); return; }
    // One sample lives in the host buffer at a time. If an older one has not been read yet (a cadence
    // tighter than the slot count), this frame is simply not sampled rather than overwriting data that a
    // later read still believes belongs to it. A pending sample that has blocked the sampler for a whole
    // slot rotation is stale - the frame it waited on never came - so it is dropped with a note instead of
    // silencing the instrument for the rest of the run.
    if (LM.pending) {
        if (++LM.stuck > 8u) {
            VKWP1_LOG("VKLUMA: pending sample never became readable (slot %u) - dropped, sampling resumes", LM.recSlot);
            LM.pending = false; LM.stuck = 0u;
        }
        vk_queue_unlock();
        return;
    }
    ++LM.frames;
    LM.stuck = 0u;
    // The first frame is always sampled: a short evidence run has to produce a number.
    if (LM.frames != 1ull && (LM.frames % LM.interval) != 0ull) { vk_queue_unlock(); return; }
    if (w < 2u || h < 2u) { vk_queue_unlock(); return; }
    const VkCommandBuffer cmd = (VkCommandBuffer)cmd_;
    const VkImage img = (VkImage)sceneImage_;
    if (!cmd || !img) { vk_queue_unlock(); return; }

    // A scene smaller than one patch still gets the whole thing; the report says which it was.
    const unsigned pw = (w < VKLUMA_PATCH) ? w : VKLUMA_PATCH;
    const unsigned ph = (h < VKLUMA_PATCH) ? h : VKLUMA_PATCH;
    unsigned n = 0;
    VkBufferImageCopy cop[VKLUMA_PATCHES];
    for (unsigned r = 0; r < VKLUMA_ROWS; ++r) {
        for (unsigned c = 0; c < VKLUMA_COLS; ++c, ++n) {
            // Patch centres sit at (c+0.5)/COLS and (r+0.5)/ROWS of the scene, then the box is pulled
            // back inside the image, which is what keeps the corner patches whole at odd resolutions.
            long x = (long)((unsigned long long)w * (2ull * c + 1ull) / (2ull * VKLUMA_COLS)) - (long)(pw / 2u);
            long y = (long)((unsigned long long)h * (2ull * r + 1ull) / (2ull * VKLUMA_ROWS)) - (long)(ph / 2u);
            if (x < 0) x = 0; if (y < 0) y = 0;
            if (x + (long)pw > (long)w) x = (long)w - (long)pw;
            if (y + (long)ph > (long)h) y = (long)h - (long)ph;
            LM.cx[n] = (unsigned)x; LM.cy[n] = (unsigned)y;
            VkBufferImageCopy cc = {};
            cc.bufferOffset = (unsigned long long)n * VKLUMA_SLOT;
            cc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            cc.imageOffset = { (int32_t)x, (int32_t)y, 0 };
            cc.imageExtent = { (uint32_t)pw, (uint32_t)ph, 1u };
            cop[n] = cc;
        }
    }
    // The scene format this layer allocates is B8G8R8A8_UNORM; the host bytes follow that order, which is
    // why the sums below are labelled B,G,R,A rather than the other way round.
    G.vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, LM.buf, n, cop);
    LM.pending = true; LM.w = w; LM.h = h; LM.pw = pw; LM.ph = ph; LM.recSlot = slot;
    vk_queue_unlock();
}

void VKCore_Luma_Poll(unsigned slot)
{
    // Readable only from the Begin that reuses the recording frame's own slot: that is the wait which
    // proves this copy finished, and no earlier Begin may touch the buffer.
    if (!LM.built || !LM.pending || LM.recSlot != slot) return;
    LM.pending = false;
    const unsigned char* base = LM.map;
    if (!base) return;

    unsigned long long sumB = 0, sumG = 0, sumR = 0, sumA = 0;
    unsigned long long total = 0, lit = 0;
    unsigned lumaMin = 255u, lumaMax = 0u;
    unsigned bins = 0;
    // The bin index below packs three bits per channel, so the table has to span 0..511. A 64-entry
    // table silently wrote up to 448 bytes past itself on the stack whenever a sampled patch contained
    // real colour, which is exactly the case the instrument exists to report.
    unsigned char seen[512] = { 0 };
    unsigned bestPatch = 0, bestMean = 0;

    // GeneralsX @feature Qoder 19/10/2026 R-02 flicker locator: the user verdict is that in-game
    // textures flicker per object, not the whole screen. A patch whose sample means repeat a,b,a,b
    // (within +-3, |a-b|>=8) over consecutive samples is exactly the signature, and camera drift
    // breaks the repeat so this is a self-filtering test. Eight samples are kept per patch; the
    // strong form (three alternating pairs over six samples) triggers the report, budgeted to keep
    // the log readable.
    static unsigned s_histL[VKLUMA_PATCHES][8];
    static unsigned s_histB[VKLUMA_PATCHES][8];
    static unsigned s_histG[VKLUMA_PATCHES][8];
    static unsigned s_histR[VKLUMA_PATCHES][8];
    static unsigned s_histN[VKLUMA_PATCHES] = { 0, 0, 0, 0, 0, 0 };
    static unsigned s_flickLast[VKLUMA_PATCHES] = { 0, 0, 0, 0, 0, 0 };
    static unsigned s_flickReports = 0;

    for (unsigned p = 0; p < VKLUMA_PATCHES; ++p) {
        const unsigned char* src = base + (unsigned long long)p * VKLUMA_SLOT;
        unsigned long long pSum = 0, pB = 0, pG = 0, pR = 0; unsigned pCount = 0;
        for (unsigned y = 0; y < LM.ph; ++y) {
            const unsigned char* row = src + (size_t)y * (size_t)LM.pw * 4u;
            for (unsigned x = 0; x < LM.pw; ++x, row += 4) {
                const unsigned b = row[0], g = row[1], r = row[2], a = row[3];
                // Rec.601 luma in 8-bit fixed point; the exact weights do not matter here, only that a
                // black frame scores 0 and a lit one does not.
                const unsigned luma = (77u * r + 150u * g + 29u * b) >> 8;
                sumB += b; sumG += g; sumR += r; sumA += a; ++total; ++pCount; pSum += luma;
                pB += b; pG += g; pR += r;
                if (luma > 8u) ++lit;
                if (luma < lumaMin) lumaMin = luma;
                if (luma > lumaMax) lumaMax = luma;
                const unsigned bin = ((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5);
                if (!seen[bin]) { seen[bin] = 1; ++bins; }
            }
        }
        const unsigned mean = pCount ? (unsigned)(pSum / pCount) : 0u;
        if (mean > bestMean) { bestMean = mean; bestPatch = p; }
        if (!pCount) continue;
        const unsigned widx = s_histN[p] & 7u;
        s_histL[p][widx] = mean;
        s_histB[p][widx] = (unsigned)(pB / pCount);
        s_histG[p][widx] = (unsigned)(pG / pCount);
        s_histR[p][widx] = (unsigned)(pR / pCount);
        ++s_histN[p];
        if (s_histN[p] < 6u || s_flickReports >= 24u || s_histN[p] - s_flickLast[p] < 8u) continue;
        // Newest-first series: v[0] is this sample, v[5] five samples back.
        unsigned v[6], hb[6], hg[6], hr[6];
        for (unsigned i = 0; i < 6u; ++i) {
            const unsigned k = (s_histN[p] - 1u - i) & 7u;
            v[i] = s_histL[p][k]; hb[i] = s_histB[p][k]; hg[i] = s_histG[p][k]; hr[i] = s_histR[p][k];
        }
        const long d02 = (long)v[0] - (long)v[2], d24 = (long)v[2] - (long)v[4];
        const long d13 = (long)v[1] - (long)v[3], d35 = (long)v[3] - (long)v[5];
        const long d01 = (long)v[0] - (long)v[1], d23 = (long)v[2] - (long)v[3], d45 = (long)v[4] - (long)v[5];
        const bool sameA = d02 <= 3 && d02 >= -3 && d24 <= 3 && d24 >= -3;
        const bool sameB = d13 <= 3 && d13 >= -3 && d35 <= 3 && d35 >= -3;
        const bool apart = (d01 >= 8 || d01 <= -8) && (d23 >= 8 || d23 <= -8) && (d45 >= 8 || d45 <= -8);
        if (!sameA || !sameB || !apart) continue;
        ++s_flickReports; s_flickLast[p] = s_histN[p];
        const unsigned lvlN = (v[0] + v[2] + v[4]) / 3u;   // newest level (v0,v2,v4)
        const unsigned lvlO = (v[1] + v[3] + v[5]) / 3u;   // other phase (v1,v3,v5)
        VKWP1_LOG("VKFLICKER patch%u@(%.0f%%,%.0f%%) luma newest-first=%u,%u,%u,%u,%u,%u levels=%u/%u "
            "newestBGRA=(%.0f,%.0f,%.0f) otherBGRA=(%.0f,%.0f,%.0f)",
            p,
            LM.w ? 100.0 * (LM.cx[p] + LM.pw / 2.0) / LM.w : 0.0,
            LM.h ? 100.0 * (LM.cy[p] + LM.ph / 2.0) / LM.h : 0.0,
            v[0], v[1], v[2], v[3], v[4], v[5], lvlN, lvlO,
            (hb[0] + hb[2] + hb[4]) / 3.0, (hg[0] + hg[2] + hg[4]) / 3.0, (hr[0] + hr[2] + hr[4]) / 3.0,
            (hb[1] + hb[3] + hb[5]) / 3.0, (hg[1] + hg[3] + hg[5]) / 3.0, (hr[1] + hr[3] + hr[5]) / 3.0);
    }
    if (!total) return;
    ++LM.reports;
    const char* verdict = (bins <= 2u) ? "flat" : (lit * 100ull < total ? "near-black" : "content");
    VKWP1_LOG("VKLUMA #%llu frame=%llu scene=%ux%u px=%u meanBGRA=(%.1f,%.1f,%.1f,%.1f) "
        "lit%%=%.1f luma[min=%u max=%u] bins=%u brightest=patch%u@(%.0f%%,%.0f%%) mean=%u verdict=%s",
        LM.reports, LM.frames, LM.w, LM.h, (unsigned)total,
        (double)sumB / (double)total, (double)sumG / (double)total,
        (double)sumR / (double)total, (double)sumA / (double)total,
        (double)lit * 100.0 / (double)total, lumaMin, lumaMax, bins, bestPatch,
        LM.w ? 100.0 * (LM.cx[bestPatch] + LM.pw / 2.0) / LM.w : 0.0,
        LM.h ? 100.0 * (LM.cy[bestPatch] + LM.ph / 2.0) / LM.h : 0.0,
        bestMean, verdict);
}

// ---- wedge 10: per-frame scene shots (GENERALSX_VKSHOT) --------------------
// GeneralsX @feature Qoder 19/10/2026 real-path forensics. The user plays CH02 from 00000000.sav and
// reports that building/terrain textures drop out to white silhouettes on some camera moves and return
// on others. Replays and patch statistics cannot show WHICH object lost its texture, so every frame is
// captured: the GPU blits the scene down to a 480x270 linear image (cheap, bilinear), copies it into a
// three-window host staging buffer at that frame's slot, and the Begin that reuses the slot (whose fence
// proves the copy landed) writes Vulkan\logs\shots\vkshot_NNNNNN.bmp plus one white/black census line.
// The census line is the time series; the BMPs are the pictures. Layout state of the small image is
// tracked by SH.layout because the record path leaves it where the next barrier must start.
namespace {

#define VKSHOT_W   480u
#define VKSHOT_H   270u
#define VKSHOT_MAX_W 3840u
#define VKSHOT_MAX_H 2160u

struct VKShot {
    bool built;
    bool dirMade;
    // GeneralsX @debug Qoder 20/10/2026 R-07: the fixed 480x270 ring cannot tell WHICH texture a striped
    // ground wedge carries - every tile is a few pixels wide there. GENERALSX_VKSHOT_W/_H raise the ring to
    // the presented extent (a forensic-tool change only; the default stays 480x270 so the older legs replay
    // byte for byte), so the capture is read at the resolution the player sees.
    unsigned w;
    unsigned h;
    VkImage img; VkDeviceMemory mem;
    VkBuffer buf; VkDeviceMemory bufMem; unsigned char* map;
    VkDeviceSize off[3];
    VkImageLayout layout;
    unsigned long long shots;
    struct Slot { bool pending; unsigned long long frame; } s[3];
};
VKShot SH = { false, false, VKSHOT_W, VKSHOT_H, nullptr, nullptr, nullptr, nullptr, nullptr, { 0, 0, 0 },
              VK_IMAGE_LAYOUT_UNDEFINED, 0ull, { { false, 0 }, { false, 0 }, { false, 0 } } };

bool vkshot_build_locked()
{
    if (SH.built) return true;
    if (!vk_env_gate("GENERALSX_VKSHOT")) return false;
    {
        const char* ew = getenv("GENERALSX_VKSHOT_W");
        const char* eh = getenv("GENERALSX_VKSHOT_H");
        const long rw = (ew && atoi(ew) > 0) ? atol(ew) : (long)VKSHOT_W;
        const long rh = (eh && atoi(eh) > 0) ? atol(eh) : (long)VKSHOT_H;
        SH.w = (unsigned)(rw < (long)VKSHOT_MAX_W ? rw : (long)VKSHOT_MAX_W);
        SH.h = (unsigned)(rh < (long)VKSHOT_MAX_H ? rh : (long)VKSHOT_MAX_H);
    }
    const VkDeviceSize shotBytes = (VkDeviceSize)SH.w * (VkDeviceSize)SH.h * 4ull;
    VkDevice dev = g_vulkanCore->Device();
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = { SH.w, SH.h, 1u };
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_LINEAR;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!chk("VKSHOT CreateImage", G.vkCreateImage(dev, &ii, nullptr, &SH.img))) return false;
    VkMemoryRequirements req = {};
    G.vkGetImageMemoryRequirements(dev, SH.img, &req);
    const uint32_t mt = vktex_memtype(req, 0);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (mt == ~0u ||
        !chk("VKSHOT AllocImageMemory", G.vkAllocateMemory(dev, &mai, nullptr, &SH.mem)) ||
        !chk("VKSHOT BindImageMemory", G.vkBindImageMemory(dev, SH.img, SH.mem, 0))) return false;

    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)shotBytes * 3ull;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("VKSHOT CreateBuffer", G.vkCreateBuffer(dev, &bi, nullptr, &SH.buf))) return false;
    G.vkGetBufferMemoryRequirements(dev, SH.buf, &req);
    const uint32_t mt2 = vktex_memtype(req,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt2;
    if (mt2 == ~0u ||
        !chk("VKSHOT AllocBufferMemory", G.vkAllocateMemory(dev, &mai, nullptr, &SH.bufMem)) ||
        !chk("VKSHOT BindBuffer", G.vkBindBufferMemory(dev, SH.buf, SH.bufMem, 0)) ||
        !chk("VKSHOT Map", G.vkMapMemory(dev, SH.bufMem, 0, bi.size, 0, (void**)&SH.map))) return false;
    for (int i = 0; i < 3; ++i) SH.off[i] = shotBytes * (unsigned long long)i;
    SH.built = true;
    VKWP1_LOG("VKSHOT on: every frame captured to Vulkan\\logs\\shots at %ux%u",
        (unsigned)SH.w, (unsigned)SH.h);
    return true;
}

void vkshot_barrier(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags srcA, VkAccessFlags dstA,
                    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = SH.img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    G.vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // anon

void VKCore_Shot_Record(void* cmd_, void* sceneImage_, unsigned w, unsigned h, unsigned slot, unsigned long long frame)
{
    if (!g_vulkanCore || !g_vulkanCore->IsReady() || slot > 2u) return;
    // GeneralsX @performance Qoder 19/10/2026 R-05: the forensic shot ring wrote one BMP per frame,
    // which at 190 fps would produce ~10000 pictures and ms-scale per-frame IO in the timed legs.
    // GENERALSX_VKSHOT_EVERY=N keeps every Nth frame (default 1 = the historical behaviour).
    {
        static unsigned s_every = 0;
        if (!s_every)
        {
            const char* ev = getenv("GENERALSX_VKSHOT_EVERY");
            s_every = (ev && atoi(ev) > 0) ? (unsigned)atoi(ev) : 1u;
        }
        if (s_every > 1u && (frame % s_every) != 0ull) return;
    }
    vk_queue_lock();
    if (!vkshot_build_locked()) { vk_queue_unlock(); return; }
    const VkCommandBuffer cmd = (VkCommandBuffer)cmd_;
    const VkImage scene = (VkImage)sceneImage_;
    if (!cmd || !scene || !w || !h) { vk_queue_unlock(); return; }
    // The scene is already TRANSFER_SRC_OPTIMAL here (the present blit and the luma copies read it from
    // this same command buffer), so only the small image needs barriers.
    vkshot_barrier(cmd, SH.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit reg = {};
    reg.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    reg.srcOffsets[0] = { 0, 0, 0 };
    reg.srcOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
    reg.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    reg.dstOffsets[0] = { 0, 0, 0 };
    reg.dstOffsets[1] = { (int32_t)SH.w, (int32_t)SH.h, 1 };
    G.vkCmdBlitImage(cmd, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        SH.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg, VK_FILTER_LINEAR);
    vkshot_barrier(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cc = {};
    cc.bufferOffset = SH.off[slot];
    cc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cc.imageExtent = { SH.w, SH.h, 1u };
    G.vkCmdCopyImageToBuffer(cmd, SH.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SH.buf, 1, &cc);
    SH.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    SH.s[slot].pending = true; SH.s[slot].frame = frame;
    vk_queue_unlock();
}


// GeneralsX @feature Qoder 22/10/2026: capture the PRESENTED image instead of the scene mirror. The
// present-side post pass (FXAA/bloom/AO/grading) writes the swapchain, so the ordinary forensic shot -
// which blits the scene mirror - cannot show any of it. This variant blits the swapchain image between
// its PRESENT_SRC barriers.
void VKCore_Shot_RecordSwap(void* cmd_, void* swapImage_, unsigned w, unsigned h, unsigned slot, unsigned long long frame)
{
    if (!g_vulkanCore || !g_vulkanCore->IsReady() || slot > 2u) return;
    {
        static unsigned s_every = 0;
        if (!s_every)
        {
            const char* ev = getenv("GENERALSX_VKSHOT_EVERY");
            s_every = (ev && atoi(ev) > 0) ? (unsigned)atoi(ev) : 1u;
        }
        if (s_every > 1u && (frame % s_every) != 0ull) return;
    }
    vk_queue_lock();
    if (!vkshot_build_locked()) { vk_queue_unlock(); return; }
    const VkCommandBuffer cmd = (VkCommandBuffer)cmd_;
    const VkImage src = (VkImage)swapImage_;
    if (!cmd || !src || !w || !h) { vk_queue_unlock(); return; }

    VkImageMemoryBarrier toBlit = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toBlit.srcAccessMask = 0;
    toBlit.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toBlit.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toBlit.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toBlit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toBlit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toBlit.image = src;
    toBlit.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    G.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toBlit);

    vkshot_barrier(cmd, SH.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit reg = {};
    reg.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    reg.srcOffsets[0] = { 0, 0, 0 };
    reg.srcOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
    reg.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    reg.dstOffsets[0] = { 0, 0, 0 };
    reg.dstOffsets[1] = { (int32_t)SH.w, (int32_t)SH.h, 1 };
    G.vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        SH.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg, VK_FILTER_LINEAR);

    VkImageMemoryBarrier toPresent = toBlit;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    G.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toPresent);

    vkshot_barrier(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cc = {};
    cc.bufferOffset = SH.off[slot];
    cc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cc.imageExtent = { SH.w, SH.h, 1u };
    G.vkCmdCopyImageToBuffer(cmd, SH.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, SH.buf, 1, &cc);
    SH.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    SH.s[slot].pending = true; SH.s[slot].frame = frame;
    vk_queue_unlock();
}

void VKCore_Shot_Poll(unsigned slot)
{
    if (!SH.built || slot > 2u || !SH.s[slot].pending) return;
    SH.s[slot].pending = false;
    const unsigned char* p = SH.map ? (const unsigned char*)SH.map + SH.off[slot] : nullptr;
    if (!p) return;
    if (!SH.dirMade) { SH.dirMade = true; CreateDirectoryA("Vulkan", nullptr); CreateDirectoryA("Vulkan\\logs", nullptr); CreateDirectoryA("Vulkan\\logs\\shots", nullptr); }
    const unsigned long long n = (unsigned long long)SH.w * (unsigned long long)SH.h;
    unsigned long long white = 0, black = 0;
    unsigned long long sum = 0, sumTop = 0;
    const unsigned long long nTop = (unsigned long long)SH.w * (SH.h / 2ull);
    // GeneralsX @debug Qoder 20/10/2026 R-06 round three: the surviving flicker dims the upper half and the
    // bottom band by different factors, and the bottom band is HUD + near terrain mixed together, so neither
    // half nor quadrant separates "the terrain receiver multiplied the scene" from "something darkens the
    // presented image". One mean per eighth of the image height gives the vertical profile per frame: the
    // row where the ratio stops being (1-strength) is the boundary of whatever darkens.
    enum { BANDS = 8u };
    const unsigned bandH = SH.h / BANDS;
    unsigned long long band[BANDS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned long long bandN[BANDS] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    unsigned x = 0, y = 0;
    for (unsigned long long i = 0; i < n; ++i) {
        const unsigned b = p[i * 4ull], g = p[i * 4ull + 1ull], r = p[i * 4ull + 2ull];
        sum += r + g + b;
        if (i < nTop) sumTop += r + g + b;
        if (r >= 240u && g >= 240u && b >= 240u) ++white;
        else if (r < 10u && g < 10u && b < 10u) ++black;
        unsigned bi = y / bandH;
        if (bi >= BANDS) bi = BANDS - 1u;
        band[bi] += r + g + b; bandN[bi] += 1ull;
        if (++x == SH.w) { x = 0u; ++y; }
    }
    char bandTxt[BANDS * 8 + 8];
    {
        unsigned o = 0;
        for (unsigned i = 0; i < BANDS; ++i)
            o += (unsigned)_snprintf(bandTxt + o, sizeof(bandTxt) - o, "%s%.1f", i ? "/" : "",
                bandN[i] ? (double)band[i] / (double)(bandN[i] * 3ull) : 0.0);
    }
    // 24-bit bottom-up BMP, rows padded to 4: 480*3=1440 is already a multiple of 4.
    unsigned char hdr[54] = { 0 };
    const unsigned rowBytes = (SH.w * 3u + 3u) & ~3u;   // BMP rows are 4-byte aligned
    const unsigned pixBytes = rowBytes * SH.h;
    hdr[0] = 'B'; hdr[1] = 'M';
    const unsigned fileSize = 54u + pixBytes;
    hdr[2] = (unsigned char)fileSize; hdr[3] = (unsigned char)(fileSize >> 8);
    hdr[4] = (unsigned char)(fileSize >> 16); hdr[5] = (unsigned char)(fileSize >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (unsigned char)SH.w; hdr[19] = (unsigned char)(SH.w >> 8);
    hdr[22] = (unsigned char)SH.h; hdr[23] = (unsigned char)(SH.h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    char path[96];
    // GeneralsX @debug Qoder 20/10/2026 R-06 second round: the flicker's true period is below the
    // 90-frame shot cadence, and every-frame BMPs would write ~25 GB in a minute. GENERALSX_VKSHOT_NOFILE=1
    // keeps the census line (the time series) and drops the picture, so GENERALSX_VKSHOT_EVERY=1 becomes
    // affordable for a frame-accurate brightness trace.
    static const bool s_nofile = (getenv("GENERALSX_VKSHOT_NOFILE") != nullptr);
    FILE* fp = nullptr;
    path[0] = 0;
    if (!s_nofile)
    {
        _snprintf(path, sizeof(path), "Vulkan\\logs\\shots\\vkshot_%06llu.bmp", SH.shots + 1ull);
        if (fopen_s(&fp, path, "wb") == 0 && fp) {
            fwrite(hdr, 1, 54, fp);
            unsigned char* line = new unsigned char[rowBytes];
            for (unsigned y = 0; y < SH.h; ++y) {
                const unsigned char* src = p + (size_t)(SH.h - 1u - y) * (size_t)SH.w * 4u;
                memset(line, 0, rowBytes);
                for (unsigned x = 0; x < SH.w; ++x) {
                    line[x * 3u] = src[x * 4u];
                    line[x * 3u + 1u] = src[x * 4u + 1u];
                    line[x * 3u + 2u] = src[x * 4u + 2u];
                }
                fwrite(line, 1, rowBytes, fp);
            }
            delete[] line;
            fclose(fp);
        } else {
            strcpy_s(path, "Vulkan\\logs\\shots\\FAILED");
        }
    }
    ++SH.shots;
    // GeneralsX @bugfix Qoder 19/10/2026 R-02 forensics: every shot line carries the constant rings'
    // wrap/spill deltas since the last shot and the FF window high-water against its capacity, so a
    // frame's white% can be read directly against what the ring did while that frame recorded. A spill
    // is the window filling and appending (safe); a wrap is the spill filling and overwriting (the
    // R-02 corruption), so the regression sentinel is wraps staying 0 while spills track the scene.
    unsigned long long cw[3] = { 0, 0, 0 }, pk[3] = { 0, 0, 0 }, cs[3] = { 0, 0, 0 };
    VKDraw_Get_Const_Wraps(cw);
    VKDraw_Get_Const_Spills(cs);
    VKDraw_Get_Const_Peaks(pk);
    static unsigned long long s_cwLast[3] = { 0, 0, 0 }, s_csLast[3] = { 0, 0, 0 };
    const unsigned long long ffCap = VKDraw_Get_Const_WindowCap(2);
    const double ffUse = ffCap ? (double)pk[2] * 100.0 / (double)ffCap : 0.0;
    VKWP1_LOG("VKSHOT #%llu frame=%llu white%%=%.2f black%%=%.2f mean=%.1f top=%.1f bands=%s cwrap=+%llu/+%llu/+%llu cum=%llu/%llu/%llu cspill=+%llu ffWin=%.1f%% of %lluKB file=%s ok=%d",
        SH.shots, SH.s[slot].frame ? SH.s[slot].frame : 0ull,
        (double)white * 100.0 / (double)n, (double)black * 100.0 / (double)n,
        (double)sum / (double)(n * 3ull), (double)sumTop / (double)(nTop * 3ull), bandTxt,
        cw[0] - s_cwLast[0], cw[1] - s_cwLast[1], cw[2] - s_cwLast[2],
        cw[0], cw[1], cw[2], cs[2] - s_csLast[2], ffUse, ffCap >> 10, path, fp ? 1 : 0);
    for (int i = 0; i < 3; ++i) { s_cwLast[i] = cw[i]; s_csLast[i] = cs[i]; }
}

// ---- wedge 5 accessors into the descriptor layer ---------------------------------------
// GeneralsX @feature Qoder 18/10/2026: VulkanDraw.cpp builds its own per-frame-slot descriptor pools
// (they must reset with the frame, which a pool shared with the mirror phase cannot) but has to reuse
// the one set layout and the two fallback resources wedge 4 created.
void* VKDesc_Set_Layout()       { return D4.built ? (void*)D4.setLayout : nullptr; }
void* VKDesc_White_View()       { return D4.built ? (void*)D4.whiteView : nullptr; }
void* VKDesc_Default_Sampler()  { return D4.built ? (void*)D4.defSampler : nullptr; }

// The co-created core for the other Vulkan translation units (declared in VulkanCore.h).
VulkanCore* VulkanCore_Get() { return g_vulkanCore; }

#endif // SAGE_USE_VULKAN_CORE || GENERALSX_VKSTANDALONE

#endif // _WIN32
