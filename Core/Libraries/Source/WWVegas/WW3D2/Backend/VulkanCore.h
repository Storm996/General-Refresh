// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 1: Vulkan device core embedded behind the DX8Wrapper facade.
// Ported verbatim in behavior from the proven standalone skeleton (engine-skeleton ran 120 frames with
// live swapchain recreates). Windows/MSVC only for now; Linux/macOS keep DXVK and never compile this TU
// (CMake gates it behind SAGE_USE_VULKAN_CORE, which the win64vk tree sets and all shipping trees leave OFF).
// Scope of this first wedge: instance/surface/device/swapchain/renderpass/framebuffers lifecycle, co-created
// next to the still-active D3D9 presentation. It draws nothing; the acceptance bar is the game launching
// with [VKWP1] lines in the console while rendering exactly as before.
#ifndef GENERALSX_VULKANCORE_H
#define GENERALSX_VULKANCORE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>
#include <vector>

class VulkanCore
{
public:
    bool Init(HWND hwnd, const char* appTag);  // full device-side setup; false on any VkResult failure
    bool Resize();                              // recreate swapchain+framebuffers (WM_SIZE / suboptimal)
    bool BeginFrame(float r, float g, float b); // wait fence, acquire image, record clear-only pass
    bool EndFrame();                            // end, submit, present; handles OUTDATED/SUBOPTIMAL
    void Shutdown();                            // ordered teardown, idempotent
    bool IsReady() const { return m_device != nullptr; }
    VkExtent2D Extent() const { return m_extent; }

    // ---- wedge 3.1: host-visible geometry ring ------------------------------------
    // A single persistently-mapped 64 MiB VkBuffer that the DX8 VB/IB lock classes mirror
    // their writes into while GENERALSX_VKGEO=1. Bump allocation with rewind-on-full; the
    // per-buffer dedicated storage + lifetime tracking arrives with wedge 5 (draw pipeline),
    // this ring only proves the host-write path and keeps lock-count conservation observable.
    bool CreateGeometryRing();                  // called from Init when GENERALSX_VKGEO is set
    bool GeoRingUp() const { return m_geoBuffer != VK_NULL_HANDLE && m_geoMap != nullptr; }
    void* Geo_Alloc(unsigned int size, bool discard); // mapped pointer, nullptr if size > capacity

    // Accessors for the wedge 3.2 texture-mirror file-static helpers (same TU).
    VkDevice       Device() const { return m_device; }
    VkQueue        Queue() const { return m_queue; }
    VkCommandPool  Pool() const { return m_pool; }
    VkPhysicalDevice Phys() const { return m_phys; }

    // ---- wedge 4 additions -------------------------------------------------------
    // Geometry ring binding support: a reservation from Geo_Alloc has to be turned back into the
    // (VkBuffer, device offset) pair vkCmdBindVertexBuffers wants. The ring is one buffer, so the
    // offset is pointer arithmetic against the mapped base; both are exposed rather than making the
    // descriptor layer a friend of the class.
    VkBuffer GeoBufferHandle() const { return m_geoBuffer; }
    unsigned long long GeoOffsetOf(const void* mapped) const
    {
        return (const char*)mapped - (const char*)m_geoMap;
    }
    // minUniformBufferOffsetAlignment, queried once at device creation (wedge 4 UBO rings).
    unsigned long long UniformAlign() const { return m_uniformAlign; }
    // Storage-buffer counterpart: the lights SSBO is bound statically, but reservations are still
    // aligned to the device requirement so a later switch to a dynamic binding needs no rework.
    unsigned long long StorageAlign() const { return m_storageAlign; }
    bool SamplerAnisotropy() const { return m_anisotropic; }
    bool SamplerMirrorClamp() const { return m_mirrorClampEdge; }
    float MaxSamplerAnisotropy() const { return m_maxAnisotropy; }

    // ---- wedge 5 additions: the draw path records into the present chain's own slots ----------
    // The frame loop owns one command buffer and one fence/semaphore triple per in-flight slot; wedge
    // 5 draws into that command buffer instead of the clear-only pass BeginFrame used to record. These
    // accessors exist so VulkanDraw.cpp can live in its own translation unit without befriending the
    // class. IN_FLIGHT is the slot count (3); a wedge-5 resize/present path must never exceed it.
    enum { VK_IN_FLIGHT = 3 };
    uint32_t    FrameIndex() const { return m_frame; }
    uint32_t    FrameSlot() const  { return m_frame % VK_IN_FLIGHT; }
    VkCommandBuffer SlotCmd(uint32_t slot) const { return m_cmds[slot]; }
    VkFence       SlotFence(uint32_t slot) const { return m_fence[slot]; }
    VkSemaphore   SlotImageAvailable(uint32_t slot) const { return m_imgAvail[slot]; }
    VkSemaphore   SlotRenderingDone(uint32_t slot) const { return m_done[slot]; }
    VkRenderPass  SwapRenderPass() const { return m_renderPass; }
    VkFramebuffer SwapFramebuffer(uint32_t image) const { return m_fbs[image]; }
    VkImage       SwapImage(uint32_t image) const { return m_images[image]; }
    VkImageView   SwapView(uint32_t image) const { return m_views[image]; }
    VkSwapchainKHR SwapHandle() const { return m_swap; }
    uint32_t      SwapImageCount() const { return (uint32_t)m_images.size(); }
    VkFormat      SwapFormat() const { return m_format; }

    // Slot count, exposed so the draw layer's own slot constant cannot drift out of sync with this one.
    static uint32_t InFlightCount() { return VK_IN_FLIGHT; }

    // ---- wedge 5 additions: frame rotation and mirror retirement ----------------------------
    // The present chain's own path advances the slot inside EndFrame. The wedge-5 draw path records
    // into these slots directly instead, so it has to advance the counter itself; without that every
    // frame reuses slot 0, the fence wait serialises the whole loop, and the rotating geometry regions
    // never rotate. Reap_Mirrors is tied to the same boundary so the two retirement queues age together.
    void Advance_Frame();

    // An evicted mirror (a recycled D3D pointer carrying a new shape, or a resize) must not be destroyed
    // while a submitted frame still names its image or view - with real draws in flight that is a device
    // loss, not a validation warning. The handles go on a retirement list and are freed once enough frame
    // boundaries have passed that no submission can still reference them.
    struct RetiredMirror {
        VkImage       image;
        VkImageView   view;
        VkDeviceMemory mem;
        unsigned      framesLeft;
    };
    void Retire_Mirror(VkImage image, VkImageView view, VkDeviceMemory mem);
    void Reap_Mirrors();
    unsigned long long MirrorsRetired() const { return m_mirRetired; }
    unsigned long long MirrorsReaped()  const { return m_mirReaped; }
    unsigned long long MirrorsWaiting() const { return (unsigned long long)m_retired.size(); }

private:
    bool loadLoader();
    bool createInstance(const char* appTag);
    bool pickPhysical();
    bool createDeviceAndQueue();
    bool createRenderPass();
    bool createSwapchain();        // create views + framebuffers too
    void destroySwapchainChain();  // framebuffers, views, swapchain
    bool importAll();

    // ---- state ----
    HMODULE          m_loader = nullptr;
    HWND             m_hwnd = nullptr;
    HINSTANCE        m_hinst = nullptr;
    VkInstance       m_instance = nullptr;
    VkSurfaceKHR     m_surface = nullptr;
    VkPhysicalDevice m_phys = nullptr;
    uint32_t         m_qfi = 0;
    VkDevice         m_device = nullptr;
    VkQueue          m_queue = nullptr;
    VkSwapchainKHR   m_swap = nullptr;
    VkFormat         m_format = VK_FORMAT_UNDEFINED;
    VkExtent2D       m_extent = {};
    VkRenderPass     m_renderPass = nullptr;
    VkCommandPool    m_pool = nullptr;
    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkFramebuffer> m_fbs;
    std::vector<VkCommandBuffer> m_cmds;   // one per in-flight slot
    VkSemaphore m_imgAvail[3] = {}, m_done[3] = {};
    VkFence     m_fence[3] = {};
    uint32_t    m_frame = 0;               // rotating slot, NOT image index
    bool        m_firstResizeDone = false;
    unsigned long long m_uniformAlign = 256; // wedge 4: queried from VkPhysicalDeviceLimits
    unsigned long long m_storageAlign = 32;  // wedge 4: minStorageBufferOffsetAlignment
    bool               m_anisotropic = false;    // samplerAnisotropy offered
    bool               m_mirrorClampEdge = false;// VK_KHR_sampler_mirror_clamp_to_edge enabled
    float              m_maxAnisotropy = 1.0f;   // maxSamplerAnisotropy

    // ---- wedge 3.1 geometry ring state ----
    VkBuffer           m_geoBuffer = VK_NULL_HANDLE;
    VkDeviceMemory     m_geoMemory = VK_NULL_HANDLE;
    void*              m_geoMap = nullptr;
    unsigned long long m_geoCap = 0;       // ring capacity in bytes
    unsigned long long m_geoCur = 0;       // bump cursor
    unsigned long long m_geoPeak = 0;      // high-water mark this session
    unsigned long long m_geoAllocs = 0;    // successful reservations
    unsigned long long m_geoRewinds = 0;   // ring-full reuse events

    // ---- wedge 5 mirror retirement queue ----
    std::vector<RetiredMirror> m_retired;
    unsigned long long m_mirRetired = 0;
    unsigned long long m_mirReaped = 0;
};

// Facade hooks used by DX8Wrapper. Both are safe no-ops unless the build defines
// SAGE_USE_VULKAN_CORE; Try_Init additionally requires env GENERALSX_VKCORE=1 so the
// default engine path (pure D3D9 semantics) is untouched even in the port tree.
bool VulkanCore_Try_Init(HWND hwnd);   // returns true only if a core was created
void VulkanCore_Shutdown();           // idempotent

// The co-created core, or nullptr. Wedge 5 split the draw path into Backend/VulkanDraw.cpp, which is
// a different translation unit and therefore cannot see the file-static singleton inside
// VulkanCore.cpp. Every Vulkan TU goes through this one accessor.
class VulkanCore;
VulkanCore* VulkanCore_Get();

// Wedge 2 hooks: with core up AND env GENERALSX_VKPRESENT=1 the Vulkan swapchain becomes
// the displayed surface; D3D9 keeps rendering to its (now unshown) backbuffer.
bool VulkanCore_Present_Enabled();
void VulkanCore_Present_Frame();      // clear-color frame through the Vulkan swapchain
void VulkanCore_Resize();             // keep swapchain extent in sync with device resets

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7 forensics: read the scene target back to the
// host and say, in the log, whether a real engine frame contains a picture. Every acceptance probe in
// this port only proves that shader arithmetic matches a CPU reference on a synthetic rig, which cannot
// distinguish "the pixel stage computes wrong colours" from "the draw path never took over" - so the
// scene gets sampled instead of another human run. Record runs at the end of a frame while the scene
// image is already in TRANSFER_SRC layout (the present blit read it from the same command buffer), Poll
// runs at the start of the next one after that slot's fence passed. Both no-op unless
// GENERALSX_VKLUMA=1; cadence is GENERALSX_VKLUMA_EVERY frames. The queue lock is re-entrant, so both
// may be called from inside an already-locked frame section.
void VKCore_Luma_Record(void* cmd, void* sceneImage, unsigned w, unsigned h, unsigned slot);
void VKCore_Luma_Poll(unsigned slot);

// GeneralsX @feature Qoder 19/10/2026 pure-Vulkan wedge 10 real-path forensics: the user verdict is that
// in-game textures drop out per object (white silhouettes that come and go with the camera) and retail
// evidence runs must be screenshots, not replays. Every frame the scene is blitted down to a fixed
// 480x270 image, copied to host staging in that frame's slot, and Poll (after the slot fence) writes a
// 24-bit BMP under Vulkan\logs\shots and logs one white/black census line per frame, so a human report
// like "shot A white, shot B normal" becomes a machine-diffable time series. No-op unless
// GENERALSX_VKSHOT=1. The queue lock is re-entrant, so both may sit inside a locked frame section.
void VKCore_Shot_Record(void* cmd, void* sceneImage, unsigned w, unsigned h, unsigned slot, unsigned long long frame);
void VKCore_Shot_RecordSwap(void* cmd, void* swapImage, unsigned w, unsigned h, unsigned slot, unsigned long long frame);
void VKCore_Shot_Poll(unsigned slot);

// GeneralsX @bugfix Qoder 19/10/2026 R-02: the present-chain frame counter, so censuses raised from the
// facade (which never sees the swapchain) can name the frame a state change belongs to. Advances once
// per presented frame in VulkanCore::Advance_Frame.
unsigned long long VKCore_Frame_No();

#endif // _WIN32
#endif // GENERALSX_VULKANCORE_H
