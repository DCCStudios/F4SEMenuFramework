// Live-process thread stack sampler. Usage: threadstack <pid> [samples]
//
// For each thread in the target process: suspend, capture context, walk the
// stack with StackWalk64 (x64 unwind data — no symbols needed), resume.
// Frames are printed as module+RVA so a hang can be attributed to a specific
// DLL (game, our plugin, another mod) even without PDBs. Threads are sampled
// multiple times so persistent (stuck) frames stand out from transient ones.
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

struct ModuleInfo {
    ULONGLONG base;
    ULONGLONG size;
    std::string name;
};

static std::vector<ModuleInfo> GetModules(HANDLE proc) {
    std::vector<ModuleInfo> mods;
    HMODULE handles[2048];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(proc, handles, sizeof(handles), &needed, LIST_MODULES_64BIT)) {
        return mods;
    }
    const int count = static_cast<int>(needed / sizeof(HMODULE));
    for (int i = 0; i < count; ++i) {
        MODULEINFO mi{};
        char name[MAX_PATH]{};
        if (GetModuleInformation(proc, handles[i], &mi, sizeof(mi)) &&
            GetModuleBaseNameA(proc, handles[i], name, sizeof(name))) {
            mods.push_back({ reinterpret_cast<ULONGLONG>(mi.lpBaseOfDll), mi.SizeOfImage, name });
        }
    }
    return mods;
}

static std::string Resolve(const std::vector<ModuleInfo>& mods, ULONGLONG addr) {
    for (const auto& m : mods) {
        if (addr >= m.base && addr < m.base + m.size) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s+0x%llx", m.name.c_str(), addr - m.base);
            return buf;
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%llx", addr);
    return buf;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: threadstack <pid> [samples]\n");
        return 2;
    }
    const DWORD pid = static_cast<DWORD>(std::atoi(argv[1]));
    const int samples = argc > 2 ? std::atoi(argv[2]) : 3;

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        std::printf("OpenProcess(%lu) failed: %lu\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    const auto mods = GetModules(proc);
    std::printf("pid %lu: %zu modules\n", pid, mods.size());

    // Collect thread ids with creation times so the earliest (main) is known.
    struct ThreadRec { DWORD tid; ULONGLONG created; };
    std::vector<ThreadRec> threads;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{ sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            ULONGLONG created = 0;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (th) {
                FILETIME c{}, e{}, k{}, u{};
                if (GetThreadTimes(th, &c, &e, &k, &u)) {
                    created = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
                }
                CloseHandle(th);
            }
            threads.push_back({ te.th32ThreadID, created });
        }
        CloseHandle(snap);
    }
    std::sort(threads.begin(), threads.end(),
              [](const ThreadRec& a, const ThreadRec& b) { return a.created < b.created; });
    std::printf("%zu threads; main tid=%lu\n\n", threads.size(), threads.empty() ? 0 : threads[0].tid);

    // stack-signature -> (count, tids) so identical waiting threads collapse.
    std::map<std::string, std::pair<int, std::string>> agg;

    for (int s = 0; s < samples; ++s) {
        for (size_t idx = 0; idx < threads.size(); ++idx) {
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                   FALSE, threads[idx].tid);
            if (!th) continue;
            if (SuspendThread(th) == static_cast<DWORD>(-1)) {
                CloseHandle(th);
                continue;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            std::string sig;
            if (GetThreadContext(th, &ctx)) {
                STACKFRAME64 frame{};
                frame.AddrPC.Offset = ctx.Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = ctx.Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = ctx.Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
                for (int f = 0; f < 24; ++f) {
                    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &frame, &ctx,
                                     nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                        break;
                    }
                    if (!frame.AddrPC.Offset) break;
                    sig += "  " + Resolve(mods, frame.AddrPC.Offset) + "\n";
                }
            }
            ResumeThread(th);
            CloseHandle(th);
            if (!sig.empty()) {
                auto& e = agg[sig];
                e.first++;
                char tidbuf[16];
                std::snprintf(tidbuf, sizeof(tidbuf), "%lu ", threads[idx].tid);
                if (e.second.find(tidbuf) == std::string::npos && e.second.size() < 120) {
                    e.second += tidbuf;
                }
            }
        }
        Sleep(300);
    }

    // Print stacks that touch interesting modules first, then the main thread,
    // then everything else by frequency.
    std::vector<std::pair<std::string, std::pair<int, std::string>>> list(agg.begin(), agg.end());
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    char mainTid[16];
    std::snprintf(mainTid, sizeof(mainTid), "%lu ", threads.empty() ? 0 : threads[0].tid);
    for (const auto& [sig, info] : list) {
        const bool ours = sig.find("F4SEMenuFramework") != std::string::npos;
        const bool isMain = info.second.find(mainTid) != std::string::npos;
        if (ours || isMain) {
            std::printf("=== %s%ssamples=%d tids=[%s]\n%s\n",
                        ours ? "[OURS] " : "", isMain ? "[MAIN] " : "",
                        info.first, info.second.c_str(), sig.c_str());
        }
    }
    std::printf("--- other threads (deduped, by frequency) ---\n");
    int printed = 0;
    for (const auto& [sig, info] : list) {
        const bool ours = sig.find("F4SEMenuFramework") != std::string::npos;
        const bool isMain = info.second.find(mainTid) != std::string::npos;
        if (ours || isMain) continue;
        if (printed++ >= 12) break;
        std::printf("samples=%d tids=[%s]\n%s\n", info.first, info.second.c_str(), sig.c_str());
    }
    SymCleanup(proc);
    CloseHandle(proc);
    return 0;
}
