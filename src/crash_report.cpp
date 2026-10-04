// crash_report.cpp — crash handler внутри csgc.dll.
//
// ВАЖНО: 0xC0000409 от __fastfail (int 29h) НЕЛЬЗЯ поймать из user-mode:
// ни SEH, ни VEH, ни SetUnhandledExceptionFilter не вызываются, ядро убивает
// процесс сразу. Поэтому для fastfail нужен WER LocalDumps (tools/enable_localdumps.bat),
// он снимает дамп извне. Этот файл ловит всё остальное и, главное, при
// CSGC_VEH=1 печатает ТИП каждого брошенного C++ исключения — что как раз
// объясняет FAST_FAIL_FATAL_APP_EXIT (terminate -> abort).
//
// Copyright (C) 2026 aka3257
// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_

#include "crash_report.h"
#include "config.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <atomic>

// ---------------------------------------------------------------------------
// Сырой лог без CRT (CRT может быть уже сломан в момент краша)
// ---------------------------------------------------------------------------
static HANDLE g_raw = INVALID_HANDLE_VALUE;

static void RawOpen(const char* path)
{
    g_raw = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void RawWrite(const char* s)
{
    if (g_raw == INVALID_HANDLE_VALUE) return;
    DWORD len = (DWORD)strlen(s);
    DWORD done = 0;
    WriteFile(g_raw, s, len, &done, nullptr);
    FlushFileBuffers(g_raw);
}

static void RawPrintf(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    RawWrite(buf);
}

// ---------------------------------------------------------------------------
// Модули
// ---------------------------------------------------------------------------
struct ModuleInfo { uint32_t base; uint32_t size; char name[MAX_PATH]; };

static const int kMaxModules = 256;
static ModuleInfo g_modules[kMaxModules];
static int g_moduleCount = 0;

static void SnapshotModules()
{
    g_moduleCount = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (g_moduleCount >= kMaxModules) break;
            ModuleInfo& m = g_modules[g_moduleCount++];
            m.base = (uint32_t)(uintptr_t)me.modBaseAddr;
            m.size = me.modBaseSize;
            WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, m.name, MAX_PATH, nullptr, nullptr);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

// Резолвер без аллокаций: VirtualQuery даёт AllocationBase (базу образа),
// GetModuleFileName по этой базе — имя. Работает даже при переполнении стека,
// когда снимок модулей через Toolhelp сделать нельзя.
static const char* ResolveAddr(uint32_t va, char* out, size_t cap)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof(mbi)) &&
        mbi.Type == MEM_IMAGE && mbi.AllocationBase) {
        char path[MAX_PATH] = { 0 };
        if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path))) {
            const char* name = strrchr(path, '\\');
            name = name ? name + 1 : path;
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X", name,
                        (unsigned)(va - (uint32_t)(uintptr_t)mbi.AllocationBase));
            return out;
        }
    }
    return nullptr;
}

static bool IsExecutable(uint32_t va)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)va, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// ---------------------------------------------------------------------------
// Minidump через системный dbghelp (не тот, что может лежать в bin игры!)
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int,
                                           void*, void*, void*);

static void WriteDump(EXCEPTION_POINTERS* ep, const char* path)
{
    // LOAD_LIBRARY_SEARCH_SYSTEM32 гарантирует, что возьмём системный dbghelp,
    // а не подсунутый рядом с exe.
    HMODULE dbg = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dbg) dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) { RawWrite("  [!] dbghelp.dll not loaded, there'll be no dump\n"); return; }

    auto writeDump = (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!writeDump) { RawWrite("  [!] MiniDumpWriteDump not found\n"); return; }

    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { RawWrite("  [!] couldn't create .dmp\n"); return; }

    MINIDUMP_EXCEPTION_INFORMATION info;
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;

    const int type = MiniDumpNormal | MiniDumpWithDataSegs |
                     MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                     MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules;

    BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, nullptr, nullptr);
    CloseHandle(f);
    RawPrintf("  dump: %s (%s)\n", path, ok ? "OK" : "ERROR");
}

// ---------------------------------------------------------------------------
// Разбор типа брошенного C++ исключения (MSVC ABI)
// ---------------------------------------------------------------------------
static bool IsReadable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return ((const uint8_t*)p + n) <= ((const uint8_t*)mbi.BaseAddress + mbi.RegionSize);
}

// В MSVC RTTI указатели бывают и абсолютными (после релокаций), и RVA.
static const void* FixupPtr(uint32_t v, const void* base)
{
    if (v == 0) return nullptr;
    if (v > 0x10000) return (const void*)(uintptr_t)v;
    return (const uint8_t*)base + v;
}

static void DescribeCxxException(EXCEPTION_RECORD* er, bool unhandled)
{
    RawPrintf("  [C++ EH] %s: code=0x%08X\n",
              unhandled ? "UNHANDLED" : "thrown", er->ExceptionCode);
    if (er->NumberParameters < 3) return;

    uint32_t magic = (uint32_t)er->ExceptionInformation[0];
    void* obj = (void*)(uintptr_t)er->ExceptionInformation[1];
    void* throwInfo = (void*)(uintptr_t)er->ExceptionInformation[2];
    RawPrintf("    magic=0x%08X object=%p ThrowInfo=%p\n", magic, obj, throwInfo);
    if (magic != 0x19930520) return;
    if (!IsReadable(throwInfo, 16)) { RawWrite("    (ThrowInfo unreadable)\n"); return; }

    // _ThrowInfo { uint32 attributes; uint32 pmfnUnwind; uint32 pForwardCompat;
    //              uint32 pCatchableTypeArray; }
    const uint32_t* ti = (const uint32_t*)throwInfo;
    const uint32_t* cta = (const uint32_t*)FixupPtr(ti[3], throwInfo);
    if (!IsReadable(cta, 8)) { RawWrite("    (no catchable type array)\n"); return; }

    RawPrintf("    catchable types: %u\n", cta[0]);
    if (cta[0] == 0) return;

    // _CatchableTypeArray { uint32 n; uint32 arrayOfTypeRva[n]; }
    const uint32_t* ct = (const uint32_t*)FixupPtr(cta[1], throwInfo);
    if (!IsReadable(ct, 8)) return;

    // _CatchableType { uint32 properties; uint32 pType; ... }
    const uint8_t* td = (const uint8_t*)FixupPtr(ct[1], throwInfo);
    if (!IsReadable(td, 16)) return;

    // _TypeDescriptor { void* pVFTable; void* spare; char name[]; }
    const char* name = (const char*)(td + 8);
    if (IsReadable(name, 2))
        RawPrintf("    type = %s\n", name);
}

// ---------------------------------------------------------------------------
// Первый проход: скан стека
// ---------------------------------------------------------------------------
static void ScanStack(uint32_t esp, uint32_t depth)
{
    RawWrite("  --- stack scan ---\n");
    const uint8_t* p = (const uint8_t*)(uintptr_t)esp;
    int printed = 0;
    for (uint32_t off = 0; off < depth && printed < 80; off += 4) {
        uint32_t v = 0;
        __try { v = *(const uint32_t*)(p + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!IsExecutable(v)) continue;
        char buf[300];
        const char* r = ResolveAddr(v, buf, sizeof(buf));
        if (r) {
            RawPrintf("    [esp+0x%03X] %08X  %s\n", off, v, r);
            printed++;
        }
    }
}

// ---------------------------------------------------------------------------
// Обработчик
// ---------------------------------------------------------------------------
static LONG g_busy = 0;
static const char* kCrashLog = "csgc_crash.log";
static const char* kCrashDump = "csgc_crash.dmp";

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&g_busy, 1)) return EXCEPTION_EXECUTE_HANDLER;

    RawOpen(kCrashLog);
    RawWrite("\n================ CSGC CRASH ================\n");
    SnapshotModules();

    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;

    RawPrintf("exception : 0x%08X (flags=0x%X, params=%u)\n",
              er->ExceptionCode, er->ExceptionFlags, er->NumberParameters);

    if (er->ExceptionCode == 0xC0000409) {
        uint32_t ff = er->NumberParameters >= 1 ? (uint32_t)er->ExceptionInformation[0] : 0;
        RawPrintf("fastfail  : %u  (%s)\n", ff,
                  ff == 7 ? "FAST_FAIL_FATAL_APP_EXIT = abort()" :
                  ff == 2 ? "STACK_COOKIE_CHECK_FAILURE" :
                  ff == 5 ? "INVALID_ARG" : "см. FAST_FAIL_*");
        RawWrite("  [!] __fastfail didn't catch in-process. WER LocalDumps needed.\n");
    }

    char buf[300];
    uint32_t addr = (uint32_t)(uintptr_t)er->ExceptionAddress;
    const char* r = ResolveAddr(addr, buf, sizeof(buf));
    RawPrintf("address   : %08X  %s\n", addr, r ? r : "(outside the modules)");

    if (er->ExceptionCode == 0xE06D7363)
        DescribeCxxException(er, true);

    for (DWORD i = 0; i < er->NumberParameters && i < 4; i++)
        RawPrintf("  param[%u] = 0x%p\n", i, (void*)er->ExceptionInformation[i]);

    if (c) {
        RawPrintf("EIP=%08X ESP=%08X EBP=%08X\n", c->Eip, c->Esp, c->Ebp);
        RawPrintf("EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\n",
                  c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
        RawWrite("  --- EBP chain ---\n");
        uint32_t ebp = c->Ebp;
        for (int i = 0; i < 40 && ebp > c->Esp && ebp < c->Esp + 0x100000; i++) {
            uint32_t nxt = 0, ret = 0;
            __try {
                nxt = *(const uint32_t*)(uintptr_t)ebp;
                ret = *(const uint32_t*)(uintptr_t)(ebp + 4);
            } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            const char* rr = ResolveAddr(ret, buf, sizeof(buf));
            if (rr) RawPrintf("    %08X  %s\n", ret, rr);
            if (nxt <= ebp) break;
            ebp = nxt;
        }
        RawWrite("  --- modules ---\n");
        for (int i = 0; i < g_moduleCount; i++)
            RawPrintf("    %08X-%08X  %s\n", g_modules[i].base,
                      g_modules[i].base + g_modules[i].size, g_modules[i].name);
        RawWrite("  --- стек ---\n");
        ScanStack(c->Esp, 0x2000);
        WriteDump(ep, kCrashDump);
    }

    RawWrite("===========================================\n");
    return EXCEPTION_EXECUTE_HANDLER;
}

static void LogStackOverflowPattern(CONTEXT* c)
{
    RawWrite("\n[SO] !!! STATUS_STACK_OVERFLOW !!!\n");
    RawPrintf("  EIP=%s\n", DescribeAddress((void*)(uintptr_t)c->Eip));
    RawPrintf("  ESP=0x%08X EBP=0x%08X\n", c->Esp, c->Ebp);

    struct Hit { uint32_t addr; int count; };
    static Hit hits[96];
    int nhits = 0;

    for (uint32_t off = 0; off < 0x20000; off += 4) {
        uint32_t va = c->Esp + off;
        if (!IsReadable((const void*)(uintptr_t)va, 4)) continue;
        uint32_t v = *(const uint32_t*)(uintptr_t)va;
        if (!IsExecutable(v)) continue;
        int i = 0;
        for (; i < nhits; i++) if (hits[i].addr == v) { hits[i].count++; break; }
        if (i == nhits && nhits < 96) { hits[nhits].addr = v; hits[nhits].count = 1; nhits++; }
    }

    // сортировка по частоте (пузырьком, элементов мало)
    for (int i = 0; i < nhits; i++)
        for (int j = i + 1; j < nhits; j++)
            if (hits[j].count > hits[i].count) { Hit t = hits[i]; hits[i] = hits[j]; hits[j] = t; }

    RawWrite("  most frequent addresses on the stack (recursion):\n");
    for (int i = 0; i < nhits && i < 12; i++) {
        const char* r = DescribeAddress((void*)(uintptr_t)hits[i].addr);
        RawPrintf("    %5d x  %s\n", hits[i].count, r);
    }

    // Главное: адрес возврата того, кто запросил гигантский кадр (_alloca_probe).
    RawWrite("  stack from ESP (what called _alloca_probe):\n");
    for (int i = 0; i < 16; i++) {
        uint32_t va = c->Esp + i * 4;
        if (!IsReadable((const void*)(uintptr_t)va, 4)) break;
        uint32_t v = *(const uint32_t*)(uintptr_t)va;
        char b2[300];
        const char* r = ResolveAddr(v, b2, sizeof(b2));
        RawPrintf("    [esp+0x%02X] = %08X  %s\n", i * 4, v, r ? r : "");
    }
    // и цепочка EBP
    RawWrite("  EBP chain:\n");
    uint32_t frame = c->Ebp;
    for (int i = 0; i < 16 && frame; i++) {
        if (!IsReadable((const void*)(uintptr_t)frame, 8)) break;
        uint32_t next = *(const uint32_t*)(uintptr_t)frame;
        uint32_t ret = *(const uint32_t*)(uintptr_t)(frame + 4);
        char b3[300];
        const char* r = ResolveAddr(ret, b3, sizeof(b3));
        if (r) RawPrintf("    %s\n", r);
        if (next <= frame) break;
        frame = next;
    }
}


static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    static std::atomic<int> s_count{ 0 };
    static bool s_opened = false;

    int n = s_count.fetch_add(1);
    if (n < 1000) {
        const DWORD code = er->ExceptionCode;
        if (code == 0x40010006 || code == 0x4001000A || code == 0x406D1388)
            return EXCEPTION_CONTINUE_SEARCH;

        if (!s_opened) { RawOpen(kCrashLog); s_opened = true; }
        if (er->ExceptionCode == 0xC00000FD && ep->ContextRecord) {
            LogStackOverflowPattern(ep->ContextRecord);
        } else if (er->ExceptionCode == 0xE06D7363) {
            DescribeCxxException(er, false);
        } else {
            RawPrintf("[VEH] code=0x%08X addr=%s params=%u\n",
                      er->ExceptionCode, DescribeAddress(er->ExceptionAddress),
                      er->NumberParameters);
            for (DWORD i = 0; i < er->NumberParameters && i < 2; i++)
                RawPrintf("      param[%u]=0x%p\n", i, (void*)er->ExceptionInformation[i]);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

const char* DescribeAddress(const void* addr)
{
    static char buf[300];
    if (g_moduleCount == 0) SnapshotModules();
    const char* r = ResolveAddr((uint32_t)(uintptr_t)addr, buf, sizeof(buf));
    if (r) return r;
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%p (outside the modules)", addr);
    return buf;
}

// ---------------------------------------------------------------------------
// Кто позвал: цепочка вызовов текущего потока
// ---------------------------------------------------------------------------
void LogStackScan(const char* tag)
{
    if (g_moduleCount == 0) SnapshotModules();

    GCLog("[STACK] --- what called %s ---\n", tag);

    uintptr_t retSlot = (uintptr_t)_AddressOfReturnAddress();
    uint32_t esp = (uint32_t)retSlot;
    uint32_t frame = esp - 4;
    char buf[300];
    int shown = 0;

    for (int i = 0; i < 24 && frame > esp; i++) {
        uint32_t next = 0, ret = 0;
        __try {
            next = *(const uint32_t*)(uintptr_t)frame;
            ret = *(const uint32_t*)(uintptr_t)(frame + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        const char* r = ResolveAddr(ret, buf, sizeof(buf));
        if (r) { GCLog("[STACK]   ebp  %08X  %s\n", ret, r); shown++; }
        if (next <= frame) break;
        frame = next;
    }

    if (shown < 3) {
        for (uint32_t off = 0; off < 0x2000 && shown <= 25; off += 4) {
            uint32_t v = 0;
            __try { v = *(const uint32_t*)(uintptr_t)(esp + off); }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (!IsExecutable(v)) continue;
            const char* r = ResolveAddr(v, buf, sizeof(buf));
            if (r) { GCLog("[STACK]   raw  %08X  %s\n", v, r); shown++; }
        }
    }
}

void InstallCrashReporter()
{
    if (CSGCConfig::NoCrashH()) return;

    DeleteFileA(kCrashLog);
    SetUnhandledExceptionFilter(CrashFilter);

    if (CSGCConfig::Veh()) {
        AddVectoredExceptionHandler(1, VehHandler);
        GCLog("[CRASH] VEH turned on — writing exceptions in %s\n", kCrashLog);
    }
    GCLog("[CRASH] crash reporter installed\n");
}
