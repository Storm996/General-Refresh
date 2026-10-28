// GeneralsX @refactor 23/10/2026: first explicit render-pass graph scaffolding.
//
// This does not reorder GPU work yet. It gives the frame one place to name its passes and measure
// them, so the next migration can move resources/barriers into the graph without changing the
// game-facing render sequence. Scopes are RAII so early returns cannot leave a dangling pass.
#pragma once

#include <cstdio>
#include <cstring>
#include <ctime>

class W3DRenderGraph
{
public:
	enum ResourceAccess
	{
		RES_READ = 1u,
		RES_WRITE = 2u
	};

	// Mirrors VKRT_Layout without pulling backend headers into the game layer.
	enum Layout
	{
		LAYOUT_UNDEFINED = 0,
		LAYOUT_TRANSFER_DST = 1,
		LAYOUT_TRANSFER_SRC = 2,
		LAYOUT_COLOR_ATTACHMENT = 3,
		LAYOUT_DEPTH_ATTACHMENT = 4,
		LAYOUT_SHADER_READ = 5,
		LAYOUT_GENERAL = 6
	};

	typedef void (*BarrierHandler)(const char* resource, int layout);

	static void SetBarrierHandler(BarrierHandler handler)
	{
		s_barrierHandler = handler;
	}

	static void Barrier(const char* resource, int layout)
	{
		if (s_barrierHandler && resource) {
			s_barrierHandler(resource, layout);
		}
	}

	// Phase 6B: the graph owns the lifetime of the frame targets, not just their logical names.
	// The backend still knows the physical format/extent rules (HDR, render scale, depth format,
	// swapchain), so the graph asks for a descriptor, then asks the backend to allocate exactly
	// those resources. Creation/free remain backend primitives; WHEN they happen is graph policy.
	typedef bool (*TargetDescHandler)(unsigned* sceneW, unsigned* sceneH, unsigned* colorFormat,
		unsigned* depthFormat, unsigned* overlayW, unsigned* overlayH, unsigned* overlayFormat,
		int* overlayWanted);
	typedef bool (*TargetAllocHandler)(const char* name, unsigned width, unsigned height,
		unsigned format, bool depth);
	typedef void (*TargetFreeHandler)(const char* name);
	// 6B: graph-owned P6 intermediates (the five bloom levels) are described by the backend each frame,
	// because only it knows whether bloom/auto-exposure is on and what the scene extent/format became.
	typedef bool (*PostTargetDescHandler)(unsigned* count, unsigned* width, unsigned* height,
		unsigned* format);
	// 6B: the water reflection image is created by the engine's render-target wrapper, then adopted by
	// the graph so its barrier/lifetime policy is graph-owned. The backend resolves the key and shape.
	typedef bool (*WaterTargetDescHandler)(void** key, unsigned* width, unsigned* height,
		unsigned* format);
	// External targets are freed by key (the graph never allocated the image itself).
	typedef void (*TargetFreeKeyHandler)(void* key);

	static void SetTargetHandlers(TargetDescHandler desc, TargetAllocHandler alloc,
		TargetFreeHandler freeHandler, TargetFreeKeyHandler freeKeyHandler)
	{
		s_targetDescHandler = desc;
		s_targetAllocHandler = alloc;
		s_targetFreeHandler = freeHandler;
		s_targetFreeKeyHandler = freeKeyHandler;
	}

	static void SetPostTargetHandler(PostTargetDescHandler handler) { s_postTargetDescHandler = handler; }
	static void SetWaterTargetHandler(WaterTargetDescHandler handler) { s_waterTargetDescHandler = handler; }

	/// Acquire this frame's sceneColor/sceneDepth (and overlay when the P6 resolve is wanted), then
	/// declare them as logical resources. A transient resource that is no longer wanted is freed here:
	/// the overlay only lives while HDR / render scale / post-AA needs it.
	// (definition below; see the PrepareTargets body)

	static W3DRenderGraph& Get()
	{
		static W3DRenderGraph graph;
		return graph;
	}

	// Logical resource tracking. This is the dependency half of the graph; barriers/lifetimes are
	// still owned by VulkanDraw. Names are interned by pointer for the frame (all call sites pass
	// string literals), so lookup is cheap and allocation-free.
	void DeclareResource(const char* name, unsigned width = 0u, unsigned height = 0u,
		unsigned format = 0u, bool transient = false)
	{
		if (!name || m_resourceCount >= 16u) return;
		for (unsigned i = 0; i < m_resourceCount; ++i) {
			if (m_resources[i].name == name || (m_resources[i].name && name && std::strcmp(m_resources[i].name, name) == 0)) {
				m_resources[i].width = width;
				m_resources[i].height = height;
				m_resources[i].format = format;
				m_resources[i].transient = transient;
				return;
			}
		}
		m_resources[m_resourceCount].name = name;
		m_resources[m_resourceCount].access = 0u;
		m_resources[m_resourceCount].written = false;
		m_resources[m_resourceCount].layout = LAYOUT_UNDEFINED;
		m_resources[m_resourceCount].width = width;
		m_resources[m_resourceCount].height = height;
		m_resources[m_resourceCount].format = format;
		m_resources[m_resourceCount].transient = transient;
		++m_resourceCount;
	}

	void UseResource(const char* name, unsigned access)
	{
		if (!name || m_resourceCount >= 16u) return;
		for (unsigned i = 0; i < m_resourceCount; ++i) {
			if (m_resources[i].name == name || (m_resources[i].name && name && std::strcmp(m_resources[i].name, name) == 0)) {
				if ((access & RES_READ) && !m_resources[i].written) {
					fprintf(stderr, "[RG] read-before-write resource='%s'\n", name);
					fflush(stderr);
				}
				m_resources[i].access |= access;
				if (access & RES_WRITE) {
					m_resources[i].written = true;
				}
				return;
			}
		}
		m_resources[m_resourceCount].name = name;
		m_resources[m_resourceCount].access = access;
		m_resources[m_resourceCount].written = (access & RES_WRITE) != 0;
		++m_resourceCount;
	}

	// Phase 6A: pass declares the layout it needs. The graph inserts a barrier only when the tracked
	// layout actually changes, and records the read/write dependency in the same call.
	void Require(const char* name, int layout, unsigned access = 0u)
	{
		if (!name) return;
		for (unsigned i = 0; i < m_resourceCount; ++i) {
			if (m_resources[i].name == name || (m_resources[i].name && std::strcmp(m_resources[i].name, name) == 0)) {
				if (m_resources[i].layout != layout) {
					Barrier(name, layout);
					m_resources[i].layout = layout;
				}
				UseResource(name, access);
				return;
			}
		}
		if (m_resourceCount >= 16u) return;
		m_resources[m_resourceCount].name = name;
		m_resources[m_resourceCount].access = access;
		m_resources[m_resourceCount].written = (access & RES_WRITE) != 0;
		m_resources[m_resourceCount].layout = layout;
		m_resources[m_resourceCount].width = 0u;
		m_resources[m_resourceCount].height = 0u;
		m_resources[m_resourceCount].format = 0u;
		m_resources[m_resourceCount].transient = false;
		Barrier(name, layout);
		++m_resourceCount;
	}

	bool PrepareTargets()
	{
		static const char* const kBloomNames[5] = { "bloom0", "bloom1", "bloom2", "bloom3", "bloom4" };
		if (!s_targetDescHandler) return false;
		unsigned sceneW = 0, sceneH = 0, colorFormat = 0, depthFormat = 0;
		unsigned overlayW = 0, overlayH = 0, overlayFormat = 0;
		int overlayWanted = 0;
		if (!s_targetDescHandler(&sceneW, &sceneH, &colorFormat, &depthFormat,
			&overlayW, &overlayH, &overlayFormat, &overlayWanted)) {
			return false;
		}
		if (!sceneW || !sceneH || !colorFormat || !depthFormat) return false;
		// Phase 6B proof line: the graph, not VulkanDraw::Begin_Frame, is the caller that asked for these.
		static int s_targetLog = 0;
		if (s_targetLog < 4) {
			++s_targetLog;
			fprintf(stderr, "[RG] targets scene=%ux%u colorFmt=%u depthFmt=%u overlay=%d %ux%u fmt=%u\n",
				sceneW, sceneH, colorFormat, depthFormat, overlayWanted, overlayW, overlayH, overlayFormat);
			fflush(stderr);
		}
		bool ok = EnsureTarget("sceneColor", sceneW, sceneH, colorFormat, false);
		ok = EnsureTarget("sceneDepth", sceneW, sceneH, depthFormat, true) && ok;
		DeclareResource("sceneColor", sceneW, sceneH, colorFormat, false);
		DeclareResource("sceneDepth", sceneW, sceneH, depthFormat, false);
		DeclareResource("shadowMap", 0u, 0u, 0u, false);
		if (overlayWanted && overlayW && overlayH && overlayFormat) {
			ok = EnsureTarget("overlay", overlayW, overlayH, overlayFormat, false) && ok;
			DeclareResource("overlay", overlayW, overlayH, overlayFormat, true);
		} else {
			// Transient lifetime: the LDR overlay exists only while the P6 resolve is needed.
			ReleaseTarget("overlay");
		}

		// 6B: P6 bloom chain (bloom0 = 1/2, .. bloom4 = 1/32). The backend decides whether the chain is
		// needed this frame (bloom intensity or auto exposure) and the exact sizes/format. The graph owns
		// allocation/release; VulkanDraw only records the blit chain into these images.
		if (s_postTargetDescHandler) {
			unsigned count = 0;
			unsigned bw[5] = {}, bh[5] = {}, bf = 0;
			if (s_postTargetDescHandler(&count, bw, bh, &bf) && bf) {
				if (count > 5u) count = 5u;
				static unsigned s_lastBloomCount = ~0u;
				if (count != s_lastBloomCount) {
					s_lastBloomCount = count;
					fprintf(stderr, "[RG] bloom chain levels=%u first=%ux%u fmt=%u\n",
						count, count ? bw[0] : 0u, count ? bh[0] : 0u, bf);
					fflush(stderr);
				}
				for (unsigned i = 0; i < 5u; ++i) {
					if (i >= count) { ReleaseTarget(kBloomNames[i]); continue; }
					ok = EnsureTarget(kBloomNames[i], bw[i], bh[i], bf, false) && ok;
					DeclareResource(kBloomNames[i], bw[i], bh[i], bf, true);
				}
			} else {
				for (unsigned i = 0; i < 5u; ++i) ReleaseTarget(kBloomNames[i]);
			}
		}

		// 6B: water reflection is an external target: the engine's render-target wrapper creates the
		// image, the graph adopts the key and owns its barrier/lifetime from then on. When the water
		// object releases it, the descriptor returns false and the graph frees the adopted image.
		if (s_waterTargetDescHandler) {
			void* waterKey = nullptr;
			unsigned ww = 0, wh = 0, wf = 0;
			if (s_waterTargetDescHandler(&waterKey, &ww, &wh, &wf) && waterKey && ww && wh) {
				AdoptTarget("waterReflection", waterKey, ww, wh, wf, false);
				DeclareResource("waterReflection", ww, wh, wf, true);
			} else {
				ReleaseTarget("waterReflection");
			}
		}
		return ok;
	}

	/// 6B: adopt an image another owner created (water reflection), so the graph can record its
	/// barriers and release it through the same lifetime path as its own targets. The image must already
	/// exist; a shape/key change is taken as a new adoption, and ReleaseTarget frees the old key first.
	bool AdoptTarget(const char* name, void* key, unsigned width, unsigned height,
		unsigned format, bool depth = false)
	{
		if (!name || !key || !width || !height) return false;
		AllocatedTarget* target = FindTarget(name);
		if (target && target->created && target->external && target->key != key) {
			ReleaseTarget(name);
			target = FindTarget(name);
		}
		if (!target) {
			if (m_targetCount >= 16u) return false;
			target = &m_targets[m_targetCount++];
			target->name = name;
			target->created = false;
			target->external = false;
			target->key = nullptr;
		}
		const bool changed = !target->created || !target->external || target->width != width ||
			target->height != height || target->format != format;
		target->width = width;
		target->height = height;
		target->format = format;
		target->depth = depth;
		target->created = true;
		target->external = true;
		target->key = key;
		if (changed) {
			static int s_adoptLog = 0;
			if (s_adoptLog < 4) {
				++s_adoptLog;
				fprintf(stderr, "[RG] adopted external target '%s' %ux%u fmt=%u\n",
					name, width, height, format);
				fflush(stderr);
			}
		}
		return true;
	}

	/// 6B: forget every target record without touching the backend. Called when the draw layer is not
	/// ready (device loss/recreate): the VK images behind the old keys are gone or about to be, and a
	/// stale `created` flag would make the graph skip the reallocation on the new device.
	void Reset()
	{
		for (unsigned i = 0; i < m_targetCount; ++i) {
			m_targets[i].created = false;
			m_targets[i].external = false;
			m_targets[i].key = nullptr;
		}
		m_targetCount = 0;
		m_resourceCount = 0;
		m_pass = 0;
		m_passName = nullptr;
	}

	const char* ResourceName(unsigned index) const
	{
		return (index < m_resourceCount) ? m_resources[index].name : nullptr;
	}
	unsigned ResourceAccess(unsigned index) const
	{
		return (index < m_resourceCount) ? m_resources[index].access : 0u;
	}

	void BeginFrame()
	{
		m_frameStart = (unsigned long long)clock();
		m_pass = 0;
		m_passName = nullptr;
		m_resourceCount = 0;
		for (unsigned i = 0; i < 16; ++i) {
			m_passNames[i] = nullptr;
			m_passMs[i] = 0.0;
			m_resources[i].name = nullptr;
			m_resources[i].access = 0u;
			m_resources[i].written = false;
			m_resources[i].layout = LAYOUT_UNDEFINED;
			m_resources[i].width = 0u;
			m_resources[i].height = 0u;
			m_resources[i].format = 0u;
			m_resources[i].transient = false;
		}
		++m_frame;
	}

	void BeginPass(const char* name)
	{
		m_passName = name;
		m_passStart = (unsigned long long)clock();
	}

	void EndPass()
	{
		if (!m_passName || m_pass >= 16) {
			m_passName = nullptr;
			return;
		}
		const unsigned long long now = (unsigned long long)clock();
		m_passNames[m_pass] = m_passName;
		m_passMs[m_pass] = (double)(now - m_passStart) * 1000.0 / (double)CLOCKS_PER_SEC;
		++m_pass;
		m_passName = nullptr;
	}

	void EndFrame()
	{
		if (m_passName) {
			EndPass();
		}
		if ((m_frame % 600u) == 0u) {
			const double totalMs = (double)((unsigned long long)clock() - m_frameStart) * 1000.0 / (double)CLOCKS_PER_SEC;
			fprintf(stderr, "[RG] frame=%u passes=%u total=%.2fms", m_frame, m_pass, totalMs);
			for (unsigned i = 0; i < m_pass; ++i) {
				fprintf(stderr, " %s=%.2f", m_passNames[i] ? m_passNames[i] : "?", m_passMs[i]);
			}
			fprintf(stderr, "\n");
			fflush(stderr);
		}
	}

private:
	// Persistent allocation records. Unlike the per-frame resource list these survive BeginFrame:
	// a target is only recreated when its extent/format changes, and a transient one is released
	// when the frame no longer wants it.
	struct AllocatedTarget {
		const char* name;
		unsigned width, height, format;
		bool depth;
		bool created;
		// 6B: external targets are images another owner created (water reflection). The graph owns the
		// barrier/lifetime policy but must free them by key, not by name through the allocator.
		bool external;
		void* key;
	};
	AllocatedTarget m_targets[16] = {};
	unsigned m_targetCount = 0;

	AllocatedTarget* FindTarget(const char* name)
	{
		for (unsigned i = 0; i < m_targetCount; ++i) {
			if (m_targets[i].name == name ||
				(m_targets[i].name && name && std::strcmp(m_targets[i].name, name) == 0)) {
				return &m_targets[i];
			}
		}
		return nullptr;
	}

	bool EnsureTarget(const char* name, unsigned width, unsigned height, unsigned format, bool depth)
	{
		if (!name || !width || !height || !s_targetAllocHandler) return false;
		AllocatedTarget* target = FindTarget(name);
		if (target && target->created && target->width == width && target->height == height &&
			target->format == format && target->depth == depth) {
			return true;
		}
		// An adopted image cannot be reallocated by the graph: its owner has to publish a new key.
		if (target && target->external) return false;
		if (!s_targetAllocHandler(name, width, height, format, depth)) {
			static int s_allocFailLog = 0;
			if (s_allocFailLog < 6) {
				++s_allocFailLog;
				fprintf(stderr, "[RG] target allocation failed: '%s' %ux%u fmt=%u depth=%d\n",
					name, width, height, format, depth ? 1 : 0);
				fflush(stderr);
			}
			return false;
		}
		if (!target) {
			if (m_targetCount >= 16u) return false;
			target = &m_targets[m_targetCount++];
			target->name = name;
			target->external = false;
			target->key = nullptr;
		}
		target->width = width;
		target->height = height;
		target->format = format;
		target->depth = depth;
		target->created = true;
		return true;
	}

	void ReleaseTarget(const char* name)
	{
		AllocatedTarget* target = FindTarget(name);
		if (target && target->created) {
			if (target->external) {
				// 6B: the image exists under an owner-provided key; free through the key handler so the
				// backend can retire (not leak) an image the graph never created.
				if (s_targetFreeKeyHandler && target->key) s_targetFreeKeyHandler(target->key);
			} else if (s_targetFreeHandler) {
				s_targetFreeHandler(name);
			}
			target->created = false;
			target->external = false;
			target->key = nullptr;
			static int s_freeLog = 0;
			if (s_freeLog < 4) {
				++s_freeLog;
				fprintf(stderr, "[RG] released transient target '%s'\n", name ? name : "?");
				fflush(stderr);
			}
		}
	}

	W3DRenderGraph() = default;
	unsigned long long m_frameStart = 0;
	unsigned long long m_passStart = 0;
	unsigned m_frame = 0;
	unsigned m_pass = 0;
	const char* m_passName = nullptr;
	const char* m_passNames[16] = {};
	double m_passMs[16] = {};
	struct Resource {
		const char* name;
		unsigned access;
		bool written;
		int layout;
		unsigned width, height, format;
		bool transient;
	};
	Resource m_resources[16] = {};
	unsigned m_resourceCount = 0;
	static BarrierHandler s_barrierHandler;
	static TargetDescHandler s_targetDescHandler;
	static TargetAllocHandler s_targetAllocHandler;
	static TargetFreeHandler s_targetFreeHandler;
	static TargetFreeKeyHandler s_targetFreeKeyHandler;
	static PostTargetDescHandler s_postTargetDescHandler;
	static WaterTargetDescHandler s_waterTargetDescHandler;
};

inline W3DRenderGraph::BarrierHandler W3DRenderGraph::s_barrierHandler = nullptr;
inline W3DRenderGraph::TargetDescHandler W3DRenderGraph::s_targetDescHandler = nullptr;
inline W3DRenderGraph::TargetAllocHandler W3DRenderGraph::s_targetAllocHandler = nullptr;
inline W3DRenderGraph::TargetFreeHandler W3DRenderGraph::s_targetFreeHandler = nullptr;
inline W3DRenderGraph::TargetFreeKeyHandler W3DRenderGraph::s_targetFreeKeyHandler = nullptr;
inline W3DRenderGraph::PostTargetDescHandler W3DRenderGraph::s_postTargetDescHandler = nullptr;
inline W3DRenderGraph::WaterTargetDescHandler W3DRenderGraph::s_waterTargetDescHandler = nullptr;

class W3DRenderFrameScope
{
public:
	W3DRenderFrameScope() { W3DRenderGraph::Get().BeginFrame(); }
	~W3DRenderFrameScope() { W3DRenderGraph::Get().EndFrame(); }
	W3DRenderFrameScope(const W3DRenderFrameScope&) = delete;
	W3DRenderFrameScope& operator=(const W3DRenderFrameScope&) = delete;
};

class W3DRenderPassScope
{
public:
	explicit W3DRenderPassScope(const char* name) { W3DRenderGraph::Get().BeginPass(name); }
	~W3DRenderPassScope() { W3DRenderGraph::Get().EndPass(); }
	W3DRenderPassScope(const W3DRenderPassScope&) = delete;
	W3DRenderPassScope& operator=(const W3DRenderPassScope&) = delete;
};
