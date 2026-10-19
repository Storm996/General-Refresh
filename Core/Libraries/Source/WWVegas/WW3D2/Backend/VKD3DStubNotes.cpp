// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J: census for the inert D3D9 stubs.
//
// VKD3DStub.h answers every D3D9 pure virtual with a body that counts the hit and returns success. That
// makes "d3d9.dll is gone and the game still runs" a *measurable* claim instead of a hope: the dump names
// which signatures the engine actually reached, and those are the exact list of methods the Vulkan backend
// must own before the no-d3d9 mode stops being a diagnostic. An entry that never appears needs no backing
// at all, so the scoreboard also bounds the work.
//
// The table is fixed size and lock-free-ish on purpose: notes can arrive from the render thread during
// static destruction, long before or after any container could be trusted to still exist.
#include "VKD3DStub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_STUB_SLOTS 512

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J4: defined with the vectored handler in
// Backend/VKD3DStubImpl.cpp, where the CPU debug registers live.
void VK_WP_Watch_Birth(void *obj);

struct VKStubSlot
{
    const char *iface;
    const char *method;
    unsigned long long hits;
};

static VKStubSlot g_vkStubSlots[VK_STUB_SLOTS];
static unsigned g_vkStubSlotCount = 0;
static unsigned long long g_vkStubTotal = 0;
static unsigned long long g_vkStubOverflow = 0;
static volatile LONG g_vkStubLock = 0;

static void vk_stub_lock()
{
    while (InterlockedCompareExchange(&g_vkStubLock, 1, 0) != 0) {
        SwitchToThread();
    }
}

static void vk_stub_unlock()
{
    InterlockedExchange(&g_vkStubLock, 0);
}

void VK_Stub_Note(const char *iface, const char *method)
{
    if (!iface || !method) return;

    ++g_vkStubTotal;

    // A run under the no-d3d9 driver usually dies on a null resource long before it reaches Shutdown, and the
    // dump is the point of the run, so it is also written on a cadence rather than only at teardown. The
    // cadence starts fine (100 hits - the boot path clears that immediately) and widens 4x each time so a
    // long run still produces readable output instead of megabytes of repeated tables.
    static unsigned long long stride = 100;
    static unsigned long long next_dump = 100;
    if (g_vkStubTotal >= next_dump) {
        if (stride < 20000) stride *= 4;
        next_dump = g_vkStubTotal + stride;
        VK_Stub_Census_Dump("periodic");
    }

    vk_stub_lock();
    for (unsigned i = 0; i < g_vkStubSlotCount; ++i) {
        if (g_vkStubSlots[i].iface == iface && g_vkStubSlots[i].method == method) {
            ++g_vkStubSlots[i].hits;
            vk_stub_unlock();
            return;
        }
    }
    if (g_vkStubSlotCount < VK_STUB_SLOTS) {
        g_vkStubSlots[g_vkStubSlotCount].iface = iface;
        g_vkStubSlots[g_vkStubSlotCount].method = method;
        g_vkStubSlots[g_vkStubSlotCount].hits = 1;
        ++g_vkStubSlotCount;
    } else {
        ++g_vkStubOverflow;
    }
    vk_stub_unlock();
}

unsigned long VK_Stub_Census_Total()
{
    return (unsigned long) g_vkStubTotal;
}

unsigned long VK_Stub_Census_Distinct()
{
    return (unsigned long) g_vkStubSlotCount;
}

void VK_Stub_Census_Dump(const char *tag)
{
    fprintf(stderr, "[VKSTUB] census reason=%s total=%llu distinct=%u overflow=%llu\n",
        tag ? tag : "?", g_vkStubTotal, g_vkStubSlotCount, g_vkStubOverflow);

    // Selection order by hit count, descending. The table is small and this runs a handful of times per
    // session, so a simple insertion pass beats a sort that has to be justified.
    unsigned order[VK_STUB_SLOTS];
    unsigned n = 0;
    for (unsigned i = 0; i < g_vkStubSlotCount; ++i) {
        unsigned pos = 0;
        while (pos < n && g_vkStubSlots[order[pos]].hits >= g_vkStubSlots[i].hits) ++pos;
        for (unsigned k = n; k > pos; --k) order[k] = order[k - 1];
        order[pos] = i;
        ++n;
    }

    const unsigned shown = n < 60u ? n : 60u;
    for (unsigned k = 0; k < shown; ++k) {
        const VKStubSlot &s = g_vkStubSlots[order[k]];
        fprintf(stderr, "[VKSTUB]   %10llu  %s::%s\n", s.hits, s.iface, s.method);
    }
    fflush(stderr);
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J3: stub COM reference forensics.
//
// The full-gate no-d3d9 run reached TextureClass::Apply and died in DX8Wrapper::Set_DX8_Texture on
// Textures[stage]->Release(): the object memory was readable but its vtable pointer read back as 0, i.e. the
// stub had already been deleted while the wrapper still held the raw pointer. Real d3d9 hides that class of
// bug because its objects are large and a freed block is not reused immediately; these stubs are tens of
// bytes, so a stale pointer hits recycled memory at once. The ring below records every transition to zero
// with the caller stack (as module RVAs, resolved by logs/_vk_symlookup.exe), and the crash handler asks for
// it, so one run names both the object and the code that dropped its last reference.
// GENERALSX_VK_TRACE_OBJ=<address> additionally traces every AddRef/Release of one object.
namespace {

struct VKStubRefEvent
{
    void *obj;
    const char *cls;
    long from;
    long to;
    unsigned long seq;
    unsigned nframes;
    void *frames[16];
};

// 4096 events is ~640 KiB of static storage, enough to cover one screen full of textures between the first
// shell-map load and the fault, so both ends of a stale object's life are still in the ring when it is read.
const unsigned VK_STUB_REF_RING = 4096;
VKStubRefEvent g_vkRefEvents[VK_STUB_REF_RING];
unsigned g_vkRefHead = 0;
unsigned long g_vkRefSeq = 0;
unsigned long long g_vkRefBirthPrints = 0;
unsigned long long g_vkRefDeathPrints = 0;
volatile LONG g_vkRefLock = 0;
int g_vkRefEnabled = -1;
void *g_vkRefTraceObj = nullptr;

inline void vk_ref_lock()
{
    while (InterlockedCompareExchange(&g_vkRefLock, 1, 0) != 0) SwitchToThread();
}

inline void vk_ref_unlock()
{
    InterlockedExchange(&g_vkRefLock, 0);
}

// Frames are stored as RVAs because the dump runs from a vectored handler, where loading symbols is not an
// option; the RVA is exactly what logs/_vk_symlookup.exe takes.
inline unsigned vk_ref_capture(void **out_frames, void *base)
{
    if (!base) return 0;
    void *raw[16];
    const USHORT n = RtlCaptureStackBackTrace(1, 16, raw, nullptr);
    for (unsigned i = 0; i < n; ++i) {
        out_frames[i] = (void *) ((const char *) raw[i] - (const char *) base);
    }
    return (unsigned) n;
}

// Reads n qwords of possibly-freed memory without dying: a short count means the block is no longer mapped,
// which is itself the answer to "was this object recycled or written over".
static unsigned vk_ref_read_block(const void *p, uintptr_t *out, unsigned n)
{
    unsigned got = 0;
    const uintptr_t *src = static_cast<const uintptr_t *>(p);
    __try {
        for (; got < n; ++got) out[got] = src[got];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        for (; got < n; ++got) out[got] = (uintptr_t) -1;
    }
    return got;
}

void vk_ref_print_frames(const VKStubRefEvent &ev)
{
    for (unsigned i = 0; i < ev.nframes; ++i) {
        fprintf(stderr, "[VKREF]   *%2u rva=0x%llx\n", i, (unsigned long long) (uintptr_t) ev.frames[i]);
    }
}

// GeneralsX @feature Qoder 18/10/2026 wedge 9.J3: live-object vtable integrity sweep.
//
// The reference ring proved the stale object had never been released: no death event exists for it. So the
// zeroed vtable pointer is not a use-after-free, it is somebody else's out-of-bounds write landing on a live
// object. A ring alone cannot see that, because it only learns about objects when their own refcount moves.
// The registry below remembers every stub that was born and not yet deleted, together with the stack that
// made it, and every few hundred reference events each live object's first qword is read again. A live object
// whose vtable pointer went to zero (or changed) is named at once, with its birth stack, which turns "the
// neighbour of an unknown writer" into a bounded window of allocations to inspect.
struct VKStubLive
{
    void *obj;
    const char *cls;
    unsigned long birth_seq;
    uintptr_t seen;          // last observed first qword; 0 until the first sweep after construction
    uintptr_t snap[6];       // first 6 qwords at the last look: vtable, refcount, then the class fields
    bool snap_valid;
    unsigned nframes;
    void *frames[16];        // birth stack, as RVAs
    bool in_use;
};

const unsigned VK_STUB_LIVE_SLOTS = 4096;
VKStubLive g_vkLive[VK_STUB_LIVE_SLOTS];
unsigned g_vkLiveHigh = 0;
unsigned long long g_vkLiveSweeps = 0;
unsigned long long g_vkCorruptReports = 0;

void vk_live_register(void *obj, const char *cls, unsigned long seq, const VKStubRefEvent &ev)
{
    for (unsigned i = 0; i < VK_STUB_LIVE_SLOTS; ++i) {
        VKStubLive &l = g_vkLive[i];
        if (l.in_use) continue;
        l.obj = obj;
        l.cls = cls ? cls : "?";
        l.birth_seq = seq;
        l.seen = 0;
        l.snap_valid = false;
        l.nframes = ev.nframes;
        for (unsigned f = 0; f < ev.nframes; ++f) l.frames[f] = ev.frames[f];
        l.in_use = true;
        if (i + 1 > g_vkLiveHigh) g_vkLiveHigh = i + 1;
        return;
    }
}

void vk_live_unregister(void *obj)
{
    for (unsigned i = 0; i < g_vkLiveHigh; ++i) {
        if (g_vkLive[i].in_use && g_vkLive[i].obj == obj) {
            g_vkLive[i].in_use = false;
            return;
        }
    }
}

// Forward: the out-slot ledger below books every store a stub makes through a caller's address. When the
// sweep finds a corrupted head, the ledger is what names the writer - or proves it was not one of ours.
void vk_store_report(const void *target, const char *why);

void vk_live_print(const VKStubLive &l, const char *why)
{
    fprintf(stderr, "[VKREF] CORRUPT (%s) %s this=%p born #%lu\n", why, l.cls, l.obj, l.birth_seq);
    for (unsigned f = 0; f < l.nframes; ++f) {
        fprintf(stderr, "[VKREF]   birth %2u rva=0x%llx\n", f, (unsigned long long) (uintptr_t) l.frames[f]);
    }
    vk_store_report(l.obj, l.cls);
}

void vk_live_sweep(void)
{
    ++g_vkLiveSweeps;
    for (unsigned i = 0; i < g_vkLiveHigh; ++i) {
        VKStubLive &l = g_vkLive[i];
        if (!l.in_use || !l.obj) continue;

        // qword 0 is the vtable pointer and qword 1 is m_vkStubRefs; the four after them are the start of the
        // class's own fields. Reading the whole head is what tells the two signatures apart: an object whose
        // head is entirely zero was recycled by the allocator, while one whose refcount and fields still look
        // alive had only its vtable qword written over by somebody else's stray store.
        uintptr_t now[6];
        vk_ref_read_block(l.obj, now, 6);
        if (now[0] == (uintptr_t) -1) {
            if (++g_vkCorruptReports <= 64ull) {
                fprintf(stderr, "[VKREF] CORRUPT (unmapped) %s this=%p born #%lu\n", l.cls, l.obj, l.birth_seq);
                fflush(stderr);
            }
            l.in_use = false;
            continue;
        }

        if (!l.snap_valid) {
            const bool headAllZero = (now[0] | now[1] | now[2] | now[3] | now[4] | now[5]) == 0;
            if (!now[0]) {
                if (++g_vkCorruptReports <= 64ull) {
                    vk_live_print(l, headAllZero ? "first look, whole head zero = block recycled"
                                                 : "first look, vtable zero but fields alive = targeted write");
                    for (unsigned q = 0; q < 6; ++q) {
                        fprintf(stderr, "[VKREF]   +0x%02x = %p\n", q * 8u, (void *) now[q]);
                    }
                    fflush(stderr);
                }
                l.in_use = false;   // an object whose memory is already gone tells us nothing more
                continue;
            }
            l.seen = now[0];
            l.snap_valid = true;
            for (unsigned q = 0; q < 6; ++q) l.snap[q] = now[q];
            continue;
        }

        // Only the vtable qword matters: refcount churn and field writes are the object working as intended,
        // and reporting them buried the corruption signal in the first run that had the whole head available.
        if (now[0] == l.snap[0]) {
            for (unsigned q = 0; q < 6; ++q) l.snap[q] = now[q];
            continue;
        }

        if (++g_vkCorruptReports <= 64ull) {
            char why[48];
            sprintf_s(why, "sweep %llu", g_vkLiveSweeps);
            vk_live_print(l, why);
            for (unsigned q = 0; q < 6; ++q) {
                if (now[q] == l.snap[q]) continue;
                fprintf(stderr, "[VKREF]   +0x%02x %p -> %p (%s)\n", q * 8u, (void *) l.snap[q], (void *) now[q],
                    q == 0u ? "vtable" : q == 1u ? "refcount" : "field");
            }
            fflush(stderr);
        }
        l.seen = now[0];
        for (unsigned q = 0; q < 6; ++q) l.snap[q] = now[q];
    }
}

}

static bool vk_ref_active()
{
    if (g_vkRefEnabled < 0) {
        const char *e = getenv("GENERALSX_VK_NOD3D");
        g_vkRefEnabled = (e && atoi(e) != 0) ? 1 : 0;
        const char *t = getenv("GENERALSX_VK_TRACE_OBJ");
        if (t) g_vkRefTraceObj = (void *) (uintptr_t) strtoull(t, nullptr, 0);
    }
    return g_vkRefEnabled != 0;
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J4: out-slot store ledger.
//
// The corruption signature is now exact: eight zero bytes land on the first qword of a live stub object,
// refcount and fields untouched. The only stores a stub performs through a caller's address are the out
// arguments it zeroes and the objects it hands back, so every one of them is booked here with the caller
// stack. A corrupted address that shows up as a store slot names the method and the call site; if the
// ledger is clean at that address, the writer is not ours and is engine-side or driver-side.
namespace {

struct VKStubStoreEvent
{
    void *slot;
    const char *what;
    unsigned long seq;
    unsigned nframes;
    void *frames[16];
};

const unsigned VK_STUB_STORE_RING = 8192;
VKStubStoreEvent g_vkStoreEvents[VK_STUB_STORE_RING];
unsigned g_vkStoreHead = 0;
unsigned long g_vkStoreSeq = 0;

}

void VK_Stub_Store_Note(void *slot, const char *what)
{
    if (!vk_ref_active() || !slot) return;

    vk_ref_lock();
    VKStubStoreEvent &ev = g_vkStoreEvents[g_vkStoreHead];
    ev.slot = slot;
    ev.what = what ? what : "?";
    ev.seq = ++g_vkStoreSeq;
    ev.nframes = vk_ref_capture(ev.frames, GetModuleHandleA(nullptr));
    g_vkStoreHead = (g_vkStoreHead + 1) % VK_STUB_STORE_RING;
    vk_ref_unlock();
}

namespace {

// A slot that points at the object itself, or at any of its first bytes, is the store that killed it.
void vk_store_report(const void *target, const char *why)
{
    const uintptr_t t = (uintptr_t) target;
    unsigned found = 0;
    for (unsigned k = 0; k < VK_STUB_STORE_RING; ++k) {
        const unsigned i = (g_vkStoreHead + k) % VK_STUB_STORE_RING;
        const VKStubStoreEvent &ev = g_vkStoreEvents[i];
        if (!ev.slot) continue;
        const uintptr_t s = (uintptr_t) ev.slot;
        if (s > t || t - s >= 16u) continue;
        ++found;
        fprintf(stderr, "[VKSTORE] #%lu slot=%p+0x%llx of %s %s -> %s\n", ev.seq, ev.slot,
            (unsigned long long) (t - s), why, ev.what, "STORED HERE");
        for (unsigned f = 0; f < ev.nframes; ++f) {
            fprintf(stderr, "[VKSTORE]   *%2u rva=0x%llx\n", f, (unsigned long long) (uintptr_t) ev.frames[f]);
        }
        if (found >= 12u) {
            fprintf(stderr, "[VKSTORE] more than 12 stores at this address, truncated\n");
            break;
        }
    }
    if (!found) fprintf(stderr, "[VKSTORE] no stub store near %s %p\n", why, target);
    fflush(stderr);
}

} // namespace

void VK_Stub_Ref(void *obj, const char *cls, long before, long after)
{
    if (!vk_ref_active() || !obj) return;

    const bool born = (before == 0 && after == 1);
    const bool dying = (after == 0);
    const bool traced = (g_vkRefTraceObj && obj == g_vkRefTraceObj);
    if (!born && !dying && !traced) return;

    VKStubRefEvent ev;
    ev.obj = obj;
    ev.cls = cls ? cls : "?";
    ev.from = before;
    ev.to = after;
    ev.nframes = vk_ref_capture(ev.frames, GetModuleHandleA(nullptr));

    vk_ref_lock();
    ev.seq = ++g_vkRefSeq;
    g_vkRefEvents[g_vkRefHead] = ev;
    g_vkRefHead = (g_vkRefHead + 1) % VK_STUB_REF_RING;
    if (born) vk_live_register(obj, cls, ev.seq, ev);
    if (dying) vk_live_unregister(obj);
    if (born) VK_WP_Watch_Birth(obj);
    const bool sweep = ((g_vkRefSeq & 63u) == 0u);
    if (sweep) vk_live_sweep();
    vk_ref_unlock();

    // The lines are printed as they happen so a hang (not just a fault) still leaves a story, but they are a
    // lot, so the flush is on a cadence and each kind is capped.
    if (born && ++g_vkRefBirthPrints <= 6000ull) {
        fprintf(stderr, "[VKREF] born #%lu %s this=%p\n", ev.seq, ev.cls, obj);
    }
    if (dying && ++g_vkRefDeathPrints <= 6000ull) {
        fprintf(stderr, "[VKREF] death #%lu %s this=%p refs %ld->0\n", ev.seq, ev.cls, obj, before);
    }
    if ((born || dying) && ((g_vkRefSeq & 63u) == 0u)) fflush(stderr);
    if (traced) {
        fprintf(stderr, "[VKREF] trace #%lu %s this=%p refs %ld->%ld\n",
            ev.seq, ev.cls, obj, before, after);
        vk_ref_print_frames(ev);
        fflush(stderr);
    }
}

void VK_Stub_Ref_Dump(const void *const *regs, unsigned nregs, const char *why)
{
    if (!vk_ref_active()) return;

    // Read the living objects one more time before reporting: the sweep that runs on the reference cadence is
    // up to 64 events behind, and the fault is the last chance to see the state the registers point at.
    vk_ref_lock();
    vk_live_sweep();
    vk_ref_unlock();

    fprintf(stderr, "[VKREF] ref ring (%s): %lu events recorded, %u live slots used\n",
        why ? why : "?", g_vkRefSeq, g_vkLiveHigh);

    // Every register that looks like a pointer is probed for its first qword. "q0=0000000000000000" on an
    // object the ring says was never deleted is the memory-corruption signature; a live vtable means the
    // register was simply stale.
    for (unsigned r = 0; r < nregs; ++r) {
        const uintptr_t v = (uintptr_t) regs[r];
        if (v < 0x10000ull || v > 0x00007FFFFFFFFFFFull) continue;
        uintptr_t head[6];
        vk_ref_read_block((const void *) v, head, 6);
        const uintptr_t q0 = head[0];
        fprintf(stderr, "[VKREF] reg %2u = %p  q0 = %p%s\n", r, regs[r], (void *) q0,
            q0 == 0 ? "  <-- vtable pointer zeroed" : (q0 == (uintptr_t) -1 ? "  <-- unreadable" : ""));
        if (q0 != 0 && q0 != (uintptr_t) -1) continue;
        for (unsigned q = 0; q < 6; ++q) {
            fprintf(stderr, "[VKREF]   r%u+0x%02x = %p%s\n", r, q * 8u, (void *) head[q],
                q == 1u ? "  (refcount)" : "");
        }
        // The block below is what the allocator wrote if the object was freed: a segregated free list keeps
        // its link in the first qwords of the block it owns, so a plausible heap pointer there means recycle.
        uintptr_t below[4];
        vk_ref_read_block((const void *) (v - 4 * sizeof(uintptr_t)), below, 4);
        for (unsigned q = 0; q < 4; ++q) {
            fprintf(stderr, "[VKREF]   r%u-0x%02x = %p (before object)\n", r, (4u - q) * 8u, (void *) below[q]);
        }
        // A zeroed register head is the corruption signature; ask the ledger whether one of our stub out-slot
        // stores landed on it. Capped: several registers usually carry the same victim and a dump must stay short.
        static unsigned s_storeReports = 0;
        if (q0 == 0 && head[1] != 0 && ++s_storeReports <= 6u) vk_store_report((const void *) v, "fault register");
    }

    // Oldest first so a life reads in order. A record whose object equals one of the fault-time registers is
    // the interesting one: that is the pointer the faulting instruction was following.
    unsigned matches = 0;
    for (unsigned k = 0; k < VK_STUB_REF_RING; ++k) {
        const unsigned i = (g_vkRefHead + k) % VK_STUB_REF_RING;
        const VKStubRefEvent &ev = g_vkRefEvents[i];
        if (!ev.obj) continue;
        int match = -1;
        for (unsigned r = 0; r < nregs; ++r) {
            if (regs[r] == ev.obj) { match = (int) r; break; }
        }
        if (match < 0) continue;
        ++matches;
        fprintf(stderr, "[VKREF] MATCH #%lu %s this=%p refs %ld->%ld (register %d)\n",
            ev.seq, ev.cls, ev.obj, ev.from, ev.to, match);
        vk_ref_print_frames(ev);
    }
    if (!matches) fprintf(stderr, "[VKREF] no ring entry matches a fault register\n");

    // The last events give the neighbourhood even without a match, and are cheap to print.
    const unsigned tail = 24u;
    for (unsigned k = VK_STUB_REF_RING - tail; k < VK_STUB_REF_RING; ++k) {
        const unsigned i = (g_vkRefHead + k) % VK_STUB_REF_RING;
        const VKStubRefEvent &ev = g_vkRefEvents[i];
        if (!ev.obj) continue;
        fprintf(stderr, "[VKREF] recent #%lu %s this=%p refs %ld->%ld\n",
            ev.seq, ev.cls, ev.obj, ev.from, ev.to);
    }
    fflush(stderr);
}
