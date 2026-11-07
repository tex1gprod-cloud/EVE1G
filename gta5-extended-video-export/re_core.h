/*
 * re_core.h - reads the Rockstar Editor's own state (GTA V Legacy / FiveM).
 *
 * Shared by Focus Puller and EVE1G (the same file is copied into both projects).
 *
 * The byte patterns and object layouts below were published by RockstarEditorPlus (CoreFX, GPL-3.0), which mapped
 * them from a dump of the running game. Only those facts are used here; the code is our own. Every address is checked
 * before use: an opcode guard on derived globals, a range check against the game image, and a montage-pointer gate on
 * the playback controller - so on a game build where a pattern drifts, the feature simply stays off.
 *
 * Read-only: nothing here changes the game. Patterns are searched for to find where the editor keeps its state, and
 * that state is read - no game code is patched and no game function that changes anything is called.
 *
 * Header-only.
 */
#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace recore
{

struct image
{
    uintptr_t base = 0, size = 0;
    bool contains(uintptr_t a) const { return a >= base && a < base + size; }
};

// --------------------------------------------------------------------------------------------------- the patterns
// IDA style, Legacy build (GTA5.exe, which FiveM runs).

// camReplayDirector::UpdateSmoothing(this) - runs once per replay frame on the game's main thread.
static const char *const PAT_UPDATESMOOTHING =
    "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 80 48 81 EC ? ? ? ? 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 "
    "48 8B D9 48 8B 89 C0 02 00 00";

// ...the same function from +0x18 on (after its pushes). Another Rockstar Editor mod (EscoEditor and the like) hooks
// UpdateSmoothing by overwriting its first bytes with a jump; the rest is untouched, so the function is still found.
static const char *const PAT_UPDATESMOOTHING_TAIL =
    "48 8D 6C 24 80 48 81 EC ? ? ? ? 0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 48 8B D9 48 8B 89 C0 02 00 00";
constexpr int UPDATESMOOTHING_TAIL_AT = 0x18;

// camReplayDirector::GetNextMarkerIndex(int) - loads the marker storage global.
static const char *const PAT_GETNEXTMARKER =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 "
    "48 8B 3D ? ? ? ? 44 8B F1 48 8B 07 48 8B CF FF 90 08 01 00 00";

// camReplayDirector::GetMaxDistanceAllowedFromPlayer(bool) - opens with cmp [CReplayMgr mode], 2.
static const char *const PAT_GETMAXDISTANCE =
    "83 3D ? ? ? ? 02 4C 8B 81 F0 02 00 00 F3 41 0F 10 40 38 75 1D 84 D2 74 19 "
    "8B 81 30 06 00 00 83 F8 FF 74 0E 39 05";

// CVideoProjectPlaybackController::JumpToNonDilatedTimeMs(float, u32) - loads the playback controller instance.
static const char *const PAT_JUMPTONONDILATED =
    "40 53 48 83 EC 40 0F 29 74 24 30 32 D2 83 3D ? ? ? ? 02 0F 29 7C 24 20 41 8B D8 0F 28 F1 75";


// --------------------------------------------------------------------------------------------------- layouts
// The playback controller (CReplayCoordinator's CReplayPlaybackController): a static that outlives projects; its
// montage pointer says whether a project is loaded.
constexpr int PBC_MONTAGE = 0x08;      // CMontage*, null with no project
constexpr int VT_CLIPINDEX = 0x50;     // s32 ()                 current clip, -1 none
constexpr int VT_CLIPCOUNT = 0x58;     // s32 ()
constexpr int VT_CLIPSTART = 0x60;     // f32 (s32 clip)         clip's own clock at its trimmed start (-1 = current)
constexpr int VT_CLIPEND = 0x68;       // f32 (s32 clip)         ... at its trimmed end
constexpr int VT_CLIPTRIMMED = 0x80;   // f32 (s32 clip)         real (dilated) length of the trimmed clip, ms
constexpr int VT_LENTOCLIP = 0x88;     // f32 (s32 clip)         real (dilated) project time where the clip starts, ms
constexpr int VT_TODILATED = 0xA0;     // f32 (s32 clip, f32 ms into clip on its own clock) -> real project ms
constexpr int VT_TONONDILATED = 0xA8;  // f32 (s32 clip, f32 real ms into clip) -> the clip's own clock
// CMontage
constexpr int MONTAGE_CLIPS = 0x00;    // CClip**
constexpr int MONTAGE_COUNT = 0x08;    // u16
constexpr int MONTAGE_NAME = 0x70;     // char[] - the project's name
constexpr int CLIP_NAME = 0x08;        // char[] - the recording's file name
// IReplayMarkerStorage
constexpr int VT_TRYGETMARKER = 0xA8;  // sReplayMarkerInfo* (u32 index)
constexpr int VT_MARKERCOUNT = 0x108;  // s32 ()
// sReplayMarkerInfo
constexpr int MARKER_TIME = 0x70;      // f32 - the clip's own clock, ms (non-dilated)
constexpr int MARKER_TYPE = 0xA0;      // u8 - 2 = anchor
constexpr int MARKER_BLEND = 0xA4;     // u8 - 0 none, 1 linear, 2 smooth
constexpr int MARKER_EASING = 0xB5;    // bool
constexpr int MARKER_FOV = 0xA3;       // u8 - the camera's field of view
constexpr int MARKER_CAMTYPE = 0xA1;   // u8 - 6 = free camera
// CReplayMgr modes
constexpr int MODE_EDIT = 2;

// --------------------------------------------------------------------------------------------------- scanning
struct pattern
{
    uint8_t bytes[160];
    uint8_t mask[160]; // 1 = must match
    int len = 0;
};

inline bool parse(const char *s, pattern &p)
{
    p.len = 0;
    while (*s && p.len < 160)
    {
        while (*s == ' ')
            ++s;
        if (!*s)
            break;
        if (*s == '?')
        {
            p.bytes[p.len] = 0;
            p.mask[p.len++] = 0;
            ++s;
            if (*s == '?')
                ++s;
            continue;
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = hex(s[0]), lo = hex(s[1]);
        if (hi < 0 || lo < 0)
            return false;
        p.bytes[p.len] = static_cast<uint8_t>(hi * 16 + lo);
        p.mask[p.len++] = 1;
        s += 2;
    }
    return p.len > 0 && p.mask[0];
}

inline bool readable(DWORD protect)
{
    if (protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
}

inline bool executable(DWORD protect)
{
    return (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

inline uintptr_t scan_range(uintptr_t from, uintptr_t to, const pattern &p)
{
    if (to - from < static_cast<uintptr_t>(p.len))
        return 0;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(from);
    const size_t n = static_cast<size_t>(to - from) - p.len + 1;
    const uint8_t first = p.bytes[0];
    for (size_t i = 0; i < n;)
    {
        const void *hit = memchr(b + i, first, n - i);
        if (!hit)
            return 0;
        i = static_cast<size_t>(static_cast<const uint8_t *>(hit) - b);
        int k = 1;
        while (k < p.len && (!p.mask[k] || b[i + k] == p.bytes[k]))
            ++k;
        if (k == p.len)
            return from + i;
        ++i;
    }
    return 0;
}

inline uintptr_t scan_region_guarded(uintptr_t from, uintptr_t to, const pattern &p)
{
    __try
    {
        return scan_range(from, to, p);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// First match of the pattern in the image's executable, readable memory. 0 = not found.
inline uintptr_t find(const image &img, const char *text)
{
    pattern p;
    if (!img.base || !parse(text, p))
        return 0;
    uintptr_t a = img.base;
    const uintptr_t end = img.base + img.size;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t re = rb + mbi.RegionSize;
        if (re > end)
            re = end;
        if (mbi.State == MEM_COMMIT && readable(mbi.Protect) && executable(mbi.Protect))
        {
            const uintptr_t hit = scan_region_guarded(a > rb ? a : rb, re, p);
            if (hit)
                return hit;
        }
        if (re <= a)
            break;
        a = re;
    }
    return 0;
}

// A global referenced by a RIP-relative instruction inside a found function: checks the opcode bytes first, then
// that the target lies inside the image. disp = offset of the rel32 from the function start, extra = bytes after it.
inline uintptr_t rip_target(const image &img, uintptr_t fn, int insn, const uint8_t *op, int op_len, int disp,
                            int extra = 0)
{
    if (!fn)
        return 0;
    __try
    {
        if (memcmp(reinterpret_cast<const void *>(fn + insn), op, op_len) != 0)
            return 0;
        const int32_t rel = *reinterpret_cast<const int32_t *>(fn + disp);
        const uintptr_t a = fn + disp + 4 + extra + static_cast<intptr_t>(rel);
        return img.contains(a) ? a : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static const uint8_t OP_LEA_RCX[] = { 0x48, 0x8D, 0x0D };            // lea rcx,[rip+d]
static const uint8_t OP_MOV_RDI[] = { 0x48, 0x8B, 0x3D };            // mov rdi,[rip+d]
static const uint8_t OP_CMPD_IMM[] = { 0x83, 0x3D };                 // cmp dword [rip+d],imm8
static const uint8_t OP_CVTTSS2SI_RCX[] = { 0xF3, 0x48, 0x0F, 0x2C, 0x0D }; // cvttss2si rcx,[rip+d]
static const uint8_t OP_CALL[] = { 0xE8 };                           // call rel32

// --------------------------------------------------------------------------------------------------- the addresses
struct addresses
{
    uintptr_t update_smoothing = 0; // function (only located: the clock global is read from its code)
    uintptr_t controller = 0;       // CReplayPlaybackController instance
    uintptr_t marker_slot = 0;      // IReplayMarkerStorage** global
    uintptr_t replay_time = 0;      // float, the current clip's clock in ms
    uintptr_t replay_mode = 0;      // int
    uintptr_t director_slot = 0;    // camReplayDirector** global (camInterface's replay director)
};

// The replay director global: every call of camReplayDirector::GetMaxDistanceAllowedFromPlayer gets its `this` either
// from `mov rcx,[rip+global]` or from camInterface's getter (`mov rax,[rip+global]; ret`). The calls are counted and
// the global most of them agree on wins (5 of 5 on b3407).
inline uintptr_t director_slot(const image &img, uintptr_t getmax)
{
    if (!getmax)
        return 0;
    uintptr_t best = 0;
    int best_n = 0;
    uintptr_t cand[16] = {};
    int votes[16] = {};
    int nc = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = img.base;
    const uintptr_t end = img.base + img.size;
    while (a < end && VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t re = rb + mbi.RegionSize;
        if (re > end)
            re = end;
        if (mbi.State == MEM_COMMIT && readable(mbi.Protect) && executable(mbi.Protect) && re - rb > 32)
        {
            __try
            {
                const uint8_t *b = reinterpret_cast<const uint8_t *>(rb);
                const size_t n = static_cast<size_t>(re - rb) - 5;
                for (size_t i = 16; i < n; ++i)
                {
                    if (b[i] != 0xE8)
                        continue;
                    const uintptr_t target = rb + i + 5 + static_cast<intptr_t>(*reinterpret_cast<const int32_t *>(b + i + 1));
                    if (target != getmax)
                        continue;
                    uintptr_t g = 0;
                    for (int back = 3; back <= 16 && !g; ++back)
                    {
                        const uint8_t *p = b + i - back;
                        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x0D) // mov rcx,[rip+d]
                            g = reinterpret_cast<uintptr_t>(p) + 7 + static_cast<intptr_t>(*reinterpret_cast<const int32_t *>(p + 3));
                        else if (back >= 5 && p[0] == 0xE8) // call getter
                        {
                            const uintptr_t f = reinterpret_cast<uintptr_t>(p) + 5 +
                                                static_cast<intptr_t>(*reinterpret_cast<const int32_t *>(p + 1));
                            if (img.contains(f) && img.contains(f + 8))
                            {
                                const uint8_t *q = reinterpret_cast<const uint8_t *>(f);
                                if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x05 && q[7] == 0xC3) // mov rax,[rip+d]; ret
                                    g = f + 7 + static_cast<intptr_t>(*reinterpret_cast<const int32_t *>(q + 3));
                            }
                            if (!g)
                                break; // another call in between: not this pattern
                        }
                    }
                    if (!g || !img.contains(g))
                        continue;
                    int k = 0;
                    while (k < nc && cand[k] != g)
                        ++k;
                    if (k == nc && nc < 16)
                        cand[nc++] = g;
                    if (k < nc && ++votes[k] > best_n)
                    {
                        best_n = votes[k];
                        best = g;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        if (re <= a)
            break;
        a = re;
    }
    return best_n >= 2 ? best : 0;
}

// Fills what it can. Returns how many were found.
inline int resolve(const image &img, addresses &a, bool want_functions = true)
{
    int n = 0;
    const uintptr_t jnd = find(img, PAT_JUMPTONONDILATED);
    if (jnd)
        a.controller = rip_target(img, jnd, 0x40, OP_LEA_RCX, 3, 0x43);
    n += a.controller != 0;
    const uintptr_t getmax = find(img, PAT_GETMAXDISTANCE);
    if (getmax)
        a.replay_mode = rip_target(img, getmax, 0x00, OP_CMPD_IMM, 2, 0x02, 1);
    if (!a.replay_mode && jnd) // that function hooked by another editor mod: JumpToNonDilated tests the same mode
        a.replay_mode = rip_target(img, jnd, 0x0D, OP_CMPD_IMM, 2, 0x0F, 1);
    n += a.replay_mode != 0;
    if (!want_functions)
        return n;
    a.update_smoothing = find(img, PAT_UPDATESMOOTHING);
    if (!a.update_smoothing)
        if (const uintptr_t t = find(img, PAT_UPDATESMOOTHING_TAIL))
            a.update_smoothing = t - UPDATESMOOTHING_TAIL_AT; // start hooked by another mod
    n += a.update_smoothing != 0;
    if (a.update_smoothing)
        a.replay_time = rip_target(img, a.update_smoothing, 0x12C, OP_CVTTSS2SI_RCX, 5, 0x131);
    n += a.replay_time != 0;
    if (const uintptr_t f = find(img, PAT_GETNEXTMARKER))
        a.marker_slot = rip_target(img, f, 0x19, OP_MOV_RDI, 3, 0x1C);
    n += a.marker_slot != 0;
    a.director_slot = director_slot(img, getmax);
    n += a.director_slot != 0;
    return n;
}

// The replay director object while the editor has one, else null.
inline const uint8_t *director(const addresses &a)
{
    if (!a.director_slot)
        return nullptr;
    __try
    {
        return *reinterpret_cast<const uint8_t *const *>(a.director_slot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// --------------------------------------------------------------------------------------------------- reading
template <int Slot, typename Fn>
inline Fn vfn(void *obj)
{
    uintptr_t *vtbl = *reinterpret_cast<uintptr_t **>(obj);
    return reinterpret_cast<Fn>(*reinterpret_cast<uintptr_t *>(reinterpret_cast<uint8_t *>(vtbl) + Slot));
}

// The controller while a project is loaded, else null.
inline void *controller(const addresses &a)
{
    if (!a.controller)
        return nullptr;
    __try
    {
        if (*reinterpret_cast<void **>(a.controller + PBC_MONTAGE) == nullptr)
            return nullptr;
        return reinterpret_cast<void *>(a.controller);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Real (dilated) length of the open project in ms - what a finished export runs for. 0 = unknown.
inline float total_ms(const addresses &a)
{
    __try
    {
        void *c = controller(a);
        if (!c)
            return 0.0f;
        const int n = vfn<VT_CLIPCOUNT, int(__fastcall *)(void *)>(c)(c);
        if (n <= 0 || n > 1024)
            return 0.0f;
        auto trimmed = vfn<VT_CLIPTRIMMED, float(__fastcall *)(void *, int)>(c);
        float total = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float d = trimmed(c, i);
            if (d > 0.0f && d < 1e8f)
                total += d;
        }
        return total;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0.0f;
    }
}

inline int replay_mode(const addresses &a)
{
    if (!a.replay_mode)
        return -1;
    __try
    {
        return *reinterpret_cast<const int *>(a.replay_mode);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// FNV-1a
inline uint64_t fnv(const char *s, int max)
{
    uint64_t h = 1469598103934665603ull;
    for (int k = 0; k < max && s[k]; ++k)
        h = (h ^ static_cast<uint8_t>(s[k])) * 1099511628211ull;
    return h;
}

// Copies a game string if it looks like one (printable, terminated, not empty).
inline bool copy_name(const char *src, char *dst, int cap)
{
    __try
    {
        for (int i = 0; i < cap - 1; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(src[i]);
            if (c == 0)
            {
                if (i == 0)
                    break;
                memcpy(dst, src, i);
                dst[i] = 0;
                return true;
            }
            if (c < 0x20 || c == 0x7F)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    dst[0] = 0;
    return false;
}

} // namespace recore
