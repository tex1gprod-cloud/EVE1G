// Camera stabilizer - see stabilizer.h for what it does and why.
#include "stabilizer.h"
#include "logger.h"

#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Misc.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <cstdio>
#include <vector>

namespace stabilizer {
namespace {

// camReplayDirector::UpdateSmoothing (the Smooth Blend spring of the Rockstar Editor's free camera). Byte signature
// as published in EVEM's build (EVE-family, Apache-2.0). The first bytes are left open: another mod may have hooked it.
const char* kUpdateSmoothingSig =
    "?? ?? ?? ?? ?? 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC ?? ?? ?? ?? "
    "0F 29 70 C8 0F 29 78 B8 44 0F 29 40 A8 48 8B D9 48 8B 89 C0 02 00 00";

struct Candidate {
    uintptr_t addr = 0;
    bool is_int = false;
    int obs = 0, hits_s = 0, hits_ms = 0;
    bool timer_s = false, timer_ms = false;
    // set while one call runs
    bool overridden = false;
    uint32_t saved = 0, written = 0;
    // for the summary
    int corrected = 0;
};

uintptr_t g_func = 0;                   // UpdateSmoothing
uint64_t g_orig = 0;                     // its trampoline (PolyHook writes it; the thunk calls through it)
std::unique_ptr<PLH::x64Detour> g_hook;
uint8_t* g_thunk = nullptr;
std::vector<Candidate> g_cands;
std::atomic<bool> g_active{false};
double g_step = 0.0;                     // export step, seconds
LARGE_INTEGER g_freq{};
LONGLONG g_last_call = 0;
double g_frame_dt = 0.0;
int g_frames = 0, g_calls = 0, g_corrected_calls = 0;
double g_dt_sum = 0.0;
uintptr_t g_mod_base = 0, g_mod_end = 0;
bool g_scanned = false;

bool writable(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) {
        return false;
    }
    const DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool readable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }
    return a + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void add_candidate(uintptr_t addr, bool is_int) {
    if (addr < g_mod_base || addr + 4 > g_mod_end || !writable(addr)) {
        return; // constants live in read-only data: only variables can be timers
    }
    for (const auto& c : g_cands) {
        if (c.addr == addr) {
            return;
        }
    }
    if (g_cands.size() < 256) {
        Candidate c;
        c.addr = addr;
        c.is_int = is_int;
        g_cands.push_back(c);
    }
}

// Walks the code of a function and collects every 32-bit value it reads from a fixed address (RIP-relative):
// SSE scalar float operations and 32-bit integer loads. Direct calls are followed one level deep.
void scan_code(uintptr_t start, size_t max_len, std::vector<uintptr_t>* callees) {
    if (!readable(start, max_len)) {
        max_len = 0x200;
        if (!readable(start, max_len)) {
            return;
        }
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(start);
    for (size_t i = 0; i + 8 < max_len; ++i) {
        // end of the function: ret followed by padding
        if (i > 0x30 && p[i] == 0xC3 && p[i + 1] == 0xCC && p[i + 2] == 0xCC) {
            break;
        }
        size_t j = i;
        const uint8_t pre = p[j];
        const bool sse_prefix = (pre == 0xF3);
        if (sse_prefix) {
            ++j;
        }
        if (p[j] >= 0x40 && p[j] <= 0x4F) {
            ++j; // REX
        }
        if (p[j] == 0x0F) {
            const uint8_t op = p[j + 1];
            const uint8_t modrm = p[j + 2];
            const bool float_op =
                sse_prefix ? (op == 0x10 || op == 0x58 || op == 0x59 || op == 0x5C || op == 0x5E || op == 0x5D ||
                              op == 0x5F || op == 0x51)
                           : (op == 0x2E || op == 0x2F);
            if (float_op && (modrm & 0xC7) == 0x05) {
                int32_t disp;
                std::memcpy(&disp, p + j + 3, 4);
                add_candidate(start + j + 7 + disp, false);
            }
        } else if (!sse_prefix) {
            // mov r32, [rip+disp32] (no REX.W: a 32-bit value, not a pointer)
            const uint8_t rex = (p[i] >= 0x40 && p[i] <= 0x4F) ? p[i] : 0;
            const size_t k = rex ? i + 1 : i;
            if (!(rex & 0x08) && p[k] == 0x8B && (p[k + 1] & 0xC7) == 0x05) {
                int32_t disp;
                std::memcpy(&disp, p + k + 2, 4);
                add_candidate(start + k + 6 + disp, true);
            }
        }
        if (callees && p[i] == 0xE8) {
            int32_t rel;
            std::memcpy(&rel, p + i + 1, 4);
            const uintptr_t target = start + i + 5 + rel;
            if (target >= g_mod_base && target < g_mod_end && callees->size() < 64 &&
                std::find(callees->begin(), callees->end(), target) == callees->end()) {
                callees->push_back(target);
            }
        }
    }
}

float value_of(const Candidate& c) {
    if (c.is_int) {
        return static_cast<float>(*reinterpret_cast<volatile int32_t*>(c.addr));
    }
    return *reinterpret_cast<volatile float*>(c.addr);
}

std::string hex_rva(uintptr_t a) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(a - g_mod_base));
    return b;
}

bool near_(double v, double want) {
    return want > 0.0 && std::fabs(v - want) <= 0.25 * want;
}

} // namespace

// Called by the thunk before UpdateSmoothing (game thread). Must not throw.
extern "C" void stabilizer_pre(void*) {
    if (!g_active.load(std::memory_order_acquire)) {
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ++g_calls;
    const double dt = g_last_call ? static_cast<double>(now.QuadPart - g_last_call) / g_freq.QuadPart : 0.0;
    g_last_call = now.QuadPart;
    const bool new_frame = dt > 0.0015; // more than one call in the same frame is possible
    if (new_frame) {
        g_frame_dt = dt;
        ++g_frames;
        if (g_frames > 2) {
            g_dt_sum += dt;
        }
    }

    // which timers follow the real frame time? (classified on real frames, before anything is changed)
    if (new_frame && g_frames > 2) {
        for (auto& c : g_cands) {
            const double v = value_of(c);
            ++c.obs;
            if (near_(v, g_frame_dt)) {
                ++c.hits_s;
            }
            if (near_(v, g_frame_dt * 1000.0)) {
                ++c.hits_ms;
            }
            if (c.obs >= 4) {
                c.timer_s = !c.is_int && c.hits_s >= 3 && c.hits_s * 10 >= c.obs * 6;
                c.timer_ms = c.hits_ms >= 3 && c.hits_ms * 10 >= c.obs * 6;
            }
        }
    }

    // give them the export step for this call
    bool any = false;
    for (auto& c : g_cands) {
        c.overridden = false;
        if (!c.timer_s && !c.timer_ms) {
            continue;
        }
        const double want = c.timer_s ? g_step : g_step * 1000.0;
        uint32_t w;
        if (c.is_int) {
            const int32_t iv = std::max(1, static_cast<int32_t>(std::lround(want)));
            std::memcpy(&w, &iv, 4);
        } else {
            const float fv = static_cast<float>(want);
            std::memcpy(&w, &fv, 4);
        }
        volatile uint32_t* slot = reinterpret_cast<volatile uint32_t*>(c.addr);
        c.saved = *slot;
        c.written = w;
        *slot = w;
        c.overridden = true;
        ++c.corrected;
        any = true;
    }
    if (any) {
        ++g_corrected_calls;
    }
}

// Called by the thunk after UpdateSmoothing: the real values go back, unless the game changed them meanwhile.
extern "C" void stabilizer_post() {
    for (auto& c : g_cands) {
        if (!c.overridden) {
            continue;
        }
        volatile uint32_t* slot = reinterpret_cast<volatile uint32_t*>(c.addr);
        if (*slot == c.written) {
            *slot = c.saved;
        }
        c.overridden = false;
    }
}

namespace {

// A small machine-code wrapper around UpdateSmoothing: keeps every argument register (integer and float) intact,
// calls stabilizer_pre, the original function, then stabilizer_post, and returns the original's result.
bool build_thunk() {
    if (g_thunk) {
        return true;
    }
    std::vector<uint8_t> c;
    auto put = [&](std::initializer_list<uint8_t> b) { c.insert(c.end(), b.begin(), b.end()); };
    auto put64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            c.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
    };
    put({0x53});                                     // push rbx
    put({0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00}); // sub rsp, 80h
    put({0x48, 0x89, 0x4C, 0x24, 0x20});             // mov [rsp+20h], rcx
    put({0x48, 0x89, 0x54, 0x24, 0x28});             // mov [rsp+28h], rdx
    put({0x4C, 0x89, 0x44, 0x24, 0x30});             // mov [rsp+30h], r8
    put({0x4C, 0x89, 0x4C, 0x24, 0x38});             // mov [rsp+38h], r9
    put({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x40});       // movdqu [rsp+40h], xmm0
    put({0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x50});       // movdqu [rsp+50h], xmm1
    put({0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x60});       // movdqu [rsp+60h], xmm2
    put({0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x70});       // movdqu [rsp+70h], xmm3
    put({0x48, 0xB8});                               // mov rax, stabilizer_pre
    put64(reinterpret_cast<uint64_t>(&stabilizer_pre));
    put({0xFF, 0xD0});                               // call rax
    put({0x48, 0x8B, 0x4C, 0x24, 0x20});             // mov rcx, [rsp+20h]
    put({0x48, 0x8B, 0x54, 0x24, 0x28});             // mov rdx, [rsp+28h]
    put({0x4C, 0x8B, 0x44, 0x24, 0x30});             // mov r8, [rsp+30h]
    put({0x4C, 0x8B, 0x4C, 0x24, 0x38});             // mov r9, [rsp+38h]
    put({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x40});       // movdqu xmm0, [rsp+40h]
    put({0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x50});       // movdqu xmm1, [rsp+50h]
    put({0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x60});       // movdqu xmm2, [rsp+60h]
    put({0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x70});       // movdqu xmm3, [rsp+70h]
    put({0x48, 0xA1});                               // mov rax, [g_orig]
    put64(reinterpret_cast<uint64_t>(&g_orig));
    put({0xFF, 0xD0});                               // call rax
    put({0x48, 0x89, 0xC3});                         // mov rbx, rax
    put({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x40});       // movdqu [rsp+40h], xmm0
    put({0x48, 0xB8});                               // mov rax, stabilizer_post
    put64(reinterpret_cast<uint64_t>(&stabilizer_post));
    put({0xFF, 0xD0});                               // call rax
    put({0x48, 0x89, 0xD8});                         // mov rax, rbx
    put({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x40});       // movdqu xmm0, [rsp+40h]
    put({0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00}); // add rsp, 80h
    put({0x5B});                                     // pop rbx
    put({0xC3});                                     // ret

    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        return false;
    }
    std::memcpy(mem, c.data(), c.size());
    // unwind data, so crash handlers and debuggers can walk the stack through the wrapper:
    // prologue = push rbx (offset 1), sub rsp, 80h (offset 8)
    {
        uint8_t* base = static_cast<uint8_t*>(mem);
        const DWORD unwind_off = 0x200, table_off = 0x240;
        uint8_t* u = base + unwind_off;
        u[0] = 1;              // version 1, no flags
        u[1] = 8;              // size of prologue
        u[2] = 2;              // two unwind codes
        u[3] = 0;              // no frame register
        u[4] = 8;              // at offset 8: UWOP_ALLOC_SMALL, (0x80 - 8) / 8 = 15
        u[5] = static_cast<uint8_t>((15 << 4) | 2);
        u[6] = 1;              // at offset 1: UWOP_PUSH_NONVOL rbx (register 3)
        u[7] = static_cast<uint8_t>((3 << 4) | 0);
        auto* rf = reinterpret_cast<RUNTIME_FUNCTION*>(base + table_off);
        rf->BeginAddress = 0;
        rf->EndAddress = static_cast<DWORD>(c.size());
        rf->UnwindData = unwind_off;
        RtlAddFunctionTable(rf, 1, reinterpret_cast<DWORD64>(base));
    }
    DWORD old = 0;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    g_thunk = static_cast<uint8_t*>(mem);
    return true;
}

bool find_function() {
    if (g_func) {
        return true;
    }
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &info, sizeof(info))) {
        LOG(LL_ERR, "Stabilizer: GetModuleInformation failed");
        return false;
    }
    g_mod_base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    g_mod_end = g_mod_base + info.SizeOfImage;
    g_func = PLH::findPattern(g_mod_base, info.SizeOfImage, kUpdateSmoothingSig);
    if (!g_func) {
        LOG(LL_NON, "Stabilizer: camReplayDirector::UpdateSmoothing not found on this game build - the camera "
                    "correction is off (exports work as before)");
        return false;
    }
    LOG(LL_NON, "Stabilizer: UpdateSmoothing at RVA ", hex_rva(g_func));
    return true;
}

void scan_candidates() {
    if (g_scanned) {
        return;
    }
    g_scanned = true;
    std::vector<uintptr_t> callees;
    scan_code(g_func, 0x2000, &callees);
    for (const uintptr_t f : callees) {
        scan_code(f, 0x400, nullptr);
    }
    LOG(LL_NON, "Stabilizer: looked at UpdateSmoothing and ", callees.size(), " functions it calls: ",
        g_cands.size(), " values that could be timers");
}

} // namespace

void Begin(double step_seconds) {
    if (g_active.load()) {
        End(); // the last export did not finish cleanly
    }
    if (step_seconds <= 0.0 || !find_function() || !build_thunk()) {
        return;
    }
    scan_candidates();
    QueryPerformanceFrequency(&g_freq);
    g_step = step_seconds;
    g_last_call = 0;
    g_frame_dt = 0.0;
    g_frames = g_calls = g_corrected_calls = 0;
    g_dt_sum = 0.0;
    for (auto& c : g_cands) {
        const uintptr_t a = c.addr;
        const bool i = c.is_int;
        c = Candidate{};
        c.addr = a;
        c.is_int = i;
    }
    if (!g_hook) {
        g_hook = std::make_unique<PLH::x64Detour>(static_cast<uint64_t>(g_func), reinterpret_cast<uint64_t>(g_thunk),
                                                  &g_orig);
        if (!g_hook->hook()) {
            LOG(LL_ERR, "Stabilizer: could not hook UpdateSmoothing - the camera correction is off");
            g_hook.reset();
            g_orig = 0;
            return;
        }
    }
    g_active.store(true, std::memory_order_release);
    LOG(LL_NON, "Stabilizer: on for this export (step ", step_seconds * 1000.0, " ms per game frame)");
}

void End() {
    if (!g_active.exchange(false)) {
        return;
    }
    if (g_hook) {
        g_hook->unHook();
        g_hook.reset();
        g_orig = 0;
    }
    int timers = 0;
    for (const auto& c : g_cands) {
        if (c.timer_s || c.timer_ms) {
            ++timers;
            LOG(LL_NON, "Stabilizer:   timer at RVA ", hex_rva(c.addr),
                c.timer_s ? " (seconds)" : " (ms)", c.is_int ? " int" : " float", " corrected ", c.corrected,
                " times");
        }
    }
    const double avg = g_frames > 2 ? g_dt_sum / (g_frames - 2) * 1000.0 : 0.0;
    LOG(LL_NON, "Stabilizer: export finished - ", g_calls, " camera smoothing calls on ", g_frames,
        " game frames, real frame time avg ", avg, " ms, export step ", g_step * 1000.0, " ms; ", timers,
        " timer(s) corrected on ", g_corrected_calls, " calls");
    if (timers == 0 && g_frames > 10) {
        LOG(LL_NON, "Stabilizer: no timer that follows the real frame time was found in UpdateSmoothing - nothing "
                    "was corrected (if the camera still shakes, send EVE1G.log)");
    }
}

} // namespace stabilizer
