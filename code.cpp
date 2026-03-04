/*
 * CLI based custom task manager for windows.
 * Developed by Jagannath Srivatsa
 * MIT License
 *
 * Features:
 *   - Process list: PID, name, CPU%, RAM (Working Set), Virtual Mem, state
 *   - Per-process memory map: base address, size, type, state, protect flags
 *   - GPU usage via DXGI Performance Counters (works NVIDIA/AMD/Intel)
 *   - Suspend / Resume a process (freezes ALL threads via NtSuspendProcess)
 *   - Timed resume: unfreeze after N seconds (background thread)
 *   - Kill with any Win32 exit code
 *   - Detailed process info: owner, priority, handles, PEB address
 *
 * Build (Developer Command Prompt / MSVC):
 *   cl /EHsc /O2 /std:c++17 task_manager_win.cpp ^
 *      /link pdh.lib psapi.lib ntdll.lib dxgi.lib ole32.lib
 *
 * Build (MinGW / g++):
 *   g++ -O2 -std=c++17 -o taskman task_manager_win.cpp ^
 *       -lpdh -lpsapi -lntdll -ldxgi -lole32
 *
 * Run (elevated for full access):
 *   taskman.exe                     (run as Administrator for best results)
 *
 * Commands:
 *   list                            refresh process list
 *   info   <pid>                    detailed process info + owner
 *   map    <pid>                    virtual memory map with hex addresses
 *   pause  <pid>                    suspend all threads (freeze)
 *   resume <pid>                    resume all threads
 *   resume <pid> <secs>             resume after N seconds
 *   kill   <pid>                    TerminateProcess (exit code 1)
 *   kill   <pid> <exitcode>         TerminateProcess with custom exit code
 *   priv                            Run in previlage mode --> access "SYSTEM" owned processes
 *   
 *   gpu                             GPU / adapter usage via DXGI
 *   help                            show this list
 *   quit / exit                     exit
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601   // Windows 7+
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shlwapi.h>
#include <conio.h>
#include <dxgi.h>
#include <wbemidl.h>
#include <comdef.h>
#include <aclapi.h>
#include <sddl.h>
#include <math.h>
#include <winternl.h>
typedef NTSTATUS(NTAPI* NtSuspendProcessFn)(HANDLE);
typedef NTSTATUS(NTAPI* NtResumeProcessFn)(HANDLE);
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <chrono>
#include <algorithm>
#include <functional>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#define RST  "\033[0m"
#define BOLD "\033[1m"
#define DIM  "\033[2m"
#define RED  "\033[31m"
#define GRN  "\033[32m"
#define YLW  "\033[33m"
#define CYN  "\033[36m"
#define MAG  "\033[35m"
#define WHT  "\033[97m"
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002L)
#define MODE "user"
bool priv_mode = false;
static void enableAnsi() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
static std::string ws2s(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], sz, nullptr, nullptr);
    return s;
}
static std::wstring s2ws(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], sz);
    return w;
}
struct ProcessInfo {
    DWORD       pid;
    std::string name;
    std::string owner;
    SIZE_T      workingSetKB;   // RAM
    SIZE_T      virtSizeKB;     // Virtual address space
    float       cpuPct;
    DWORD       priority;
    DWORD       handleCount;
    std::string priorityName;
};
struct MemRegion {
    uintptr_t   baseAddr;
    SIZE_T      regionSize;
    std::string state;      // Free / Reserve / Commit
    std::string type;       // Image / Mapped / Private
    std::string protect;    // rwx flags
    std::string mappedFile; // if Image/Mapped
};
struct GpuInfo {
    std::string name;
    SIZE_T      dedicatedVRAM_MB;
    SIZE_T      sharedRAM_MB;
};
struct CpuCounter {
    PDH_HQUERY   query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool         ok = false;
};
static std::map<DWORD, CpuCounter> cpuCounters;

//HANDLE opened and closed 17 times. Am I a noob? let me write a context aware handle function that closes immediately when out of scope
class myHandle {
    HANDLE handle = nullptr;
public:
    myHandle() = default;
    explicit myHandle(HANDLE h)
     : handle(h) {}
    myHandle(const myHandle&) = delete;
    myHandle& operator=(const myHandle&) = delete;
    myHandle(myHandle&& other) noexcept
        : handle(other.handle)
    {
        other.handle = nullptr;
    }
    myHandle& operator=(myHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~myHandle()
    {
        reset();
    }
    void reset(HANDLE newHandle = nullptr)
    {
        if (handle && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
        handle = newHandle;
    }
    HANDLE get() const
    {
        return handle;
    }
    operator HANDLE() const
    {
        return handle;
    }
    bool valid() const
    {
        return handle && handle != INVALID_HANDLE_VALUE;
    }
};

static float getCpuPct(DWORD pid) {
    // Build counter path  \Process(<name>:N)\% Processor Time
    // Simple approach: use NtQuerySystemInformation delta for better accuracy.
    // Here we use a lightweight GetProcessTimes delta stored per-PID.
    // (PDH per-instance requires instance name disambiguation — complex.)
    // Instead: snapshot FILETIME twice with ~0 interval gives instantaneous.
    static std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> prev; // pid -> {proc, sys}

    //HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
    myHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid));
    if (!h) return 0.f;

    FILETIME ct, et, kt, ut;
    if (!GetProcessTimes(h, &ct, &et, &kt, &ut)) { CloseHandle(h); return 0.f; }
    //CloseHandle(h); Not required now due to myHandle implementation
    ULONGLONG proc = ((ULONGLONG)kt.dwHighDateTime << 32 | kt.dwLowDateTime)
        + ((ULONGLONG)ut.dwHighDateTime << 32 | ut.dwLowDateTime);

    FILETIME idleF, kernF, userF;
    GetSystemTimes(&idleF, &kernF, &userF);
    ULONGLONG sys = ((ULONGLONG)kernF.dwHighDateTime << 32 | kernF.dwLowDateTime)
        + ((ULONGLONG)userF.dwHighDateTime << 32 | userF.dwLowDateTime);

    if (!prev.count(pid)) { prev[pid] = { proc, sys }; return 0.f; }

    ULONGLONG dp = proc - prev[pid].first;
    ULONGLONG ds = sys - prev[pid].second;
    prev[pid] = { proc, sys };
    if (ds == 0) return 0.f;
    float pct = 100.f * (float)dp / (float)ds;
    return pct < 100.f ? pct : 100.f;
}
static std::string getOwner(DWORD pid) {
    myHandle hProc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid));
    if (!hProc) return "?";
    HANDLE rawTok = nullptr;
    if (!OpenProcessToken(hProc, TOKEN_QUERY, &rawTok)) { return "?"; }
    myHandle hTok(rawTok);
    DWORD sz = 0;
    GetTokenInformation(hTok, TokenUser, nullptr, 0, &sz);
    std::vector<BYTE> buf(sz);
    std::string result = "?";
    if (GetTokenInformation(hTok, TokenUser, buf.data(), sz, &sz)) {
        TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        wchar_t name[256] = {}, dom[256] = {};
        DWORD nl = 256, dl = 256;
        SID_NAME_USE use;
        if (LookupAccountSidW(nullptr, tu->User.Sid, name, &nl, dom, &dl, &use))
            result = ws2s(dom) + "\\" + ws2s(name);
    }
    //CloseHandle(hTok);
    return result;
}
static std::string priorityStr(DWORD p) {
    switch (p) {
    case IDLE_PRIORITY_CLASS:          return "Idle";
    case BELOW_NORMAL_PRIORITY_CLASS:  return "BelowNormal";
    case NORMAL_PRIORITY_CLASS:        return "Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS:  return "AboveNormal";
    case HIGH_PRIORITY_CLASS:          return "High";
    case REALTIME_PRIORITY_CLASS:      return "Realtime";
    default: return std::to_string(p);
    }
}
static std::vector<ProcessInfo> enumProcesses() {
    std::vector<ProcessInfo> procs;
    myHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snap == INVALID_HANDLE_VALUE) return procs;

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (!Process32FirstW(snap, &pe)) { return procs; }

    do {
        ProcessInfo p;
        p.pid = pe.th32ProcessID;
        p.name = ws2s(pe.szExeFile);

        myHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            false, p.pid));
        if (h) {
            PROCESS_MEMORY_COUNTERS_EX pmc{ sizeof(pmc) };
            if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                p.workingSetKB = pmc.WorkingSetSize / 1024;
                p.virtSizeKB = pmc.PrivateUsage / 1024;
            }
            p.priority = GetPriorityClass(h);
            p.priorityName = priorityStr(p.priority);
            GetProcessHandleCount(h, &p.handleCount);
            //CloseHandle(h);
        }
        p.cpuPct = getCpuPct(p.pid);
        p.owner = getOwner(p.pid);
        procs.push_back(p);
    } while (Process32NextW(snap, &pe));

    //CloseHandle(snap);
    std::sort(procs.begin(), procs.end(),
        [](const ProcessInfo& a, const ProcessInfo& b) {
            return a.workingSetKB > b.workingSetKB; });
    return procs;
}
static std::string protStr(DWORD p) {
    // Strip modifiers
    p &= ~(PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE);
    switch (p) {
    case PAGE_NOACCESS:          return "---";
    case PAGE_READONLY:          return "r--";
    case PAGE_READWRITE:         return "rw-";
    case PAGE_WRITECOPY:         return "rw(cow)";
    case PAGE_EXECUTE:           return "--x";
    case PAGE_EXECUTE_READ:      return "r-x";
    case PAGE_EXECUTE_READWRITE: return "rwx";
    case PAGE_EXECUTE_WRITECOPY: return "rwx(cow)";
    default:                     return "???";
    }
}
static std::string stateStr(DWORD s) {
    if (s == MEM_FREE)    return "Free";
    if (s == MEM_RESERVE) return "Reserve";
    if (s == MEM_COMMIT)  return "Commit";
    return "?";
}
static std::string typeStr(DWORD t) {
    if (t == MEM_IMAGE)   return "Image";
    if (t == MEM_MAPPED)  return "Mapped";
    if (t == MEM_PRIVATE) return "Private";
    return "-";
}

static std::vector<MemRegion> getMemMap(DWORD pid) {
    std::vector<MemRegion> regions;
    myHandle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid));
    if (!h) return regions;

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0;
    while (VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        MemRegion r;
        r.baseAddr = (uintptr_t)mbi.BaseAddress;
        r.regionSize = mbi.RegionSize;
        r.state = stateStr(mbi.State);
        r.type = typeStr(mbi.Type);
        r.protect = (mbi.State == MEM_COMMIT) ? protStr(mbi.Protect) : "";

        // Try to get mapped filename
        if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) {
            wchar_t fname[MAX_PATH] = {};
            if (GetMappedFileNameW(h, mbi.BaseAddress, fname, MAX_PATH)) {
                // Convert device path \Device\HarddiskVolume1\... to C:\...
                std::wstring wp = fname;
                wchar_t drives[512] = {};
                GetLogicalDriveStringsW(511, drives);
                for (wchar_t* d = drives; *d; d += wcslen(d) + 1) {
                    wchar_t dev[MAX_PATH] = {};
                    std::wstring dl = std::wstring(d).substr(0, 2);
                    if (QueryDosDeviceW(dl.c_str(), dev, MAX_PATH)) {
                        std::wstring devPath = dev;
                        if (wp.find(devPath) == 0) {
                            wp = dl + wp.substr(devPath.size());
                            break;
                        }
                    }
                }
                r.mappedFile = ws2s(wp);
            }
        }
        regions.push_back(r);
        addr = r.baseAddr + r.regionSize;
        if (addr == 0) break; // wrapped around (32-bit on 64-bit)
    }
    //CloseHandle(h);
    return regions;
}

static std::vector<GpuInfo> queryGpus() {
    std::vector<GpuInfo> gpus;
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)))
        return gpus;
    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        GpuInfo g;
        g.name = ws2s(desc.Description);
        g.dedicatedVRAM_MB = (SIZE_T)(desc.DedicatedVideoMemory / 1024 / 1024);
        g.sharedRAM_MB = (SIZE_T)(desc.SharedSystemMemory / 1024 / 1024);
        gpus.push_back(g);
        adapter->Release();
    }
    factory->Release();
    return gpus;
}
static NtSuspendProcessFn fnSuspend = nullptr;
static NtResumeProcessFn  fnResume = nullptr;
static void loadNtFuncs() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    fnSuspend = (NtSuspendProcessFn)GetProcAddress(ntdll, "NtSuspendProcess");
    fnResume = (NtResumeProcessFn)GetProcAddress(ntdll, "NtResumeProcess");
}
static bool isAdmin() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return false;
    myHandle hToken(rawToken);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool result = GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size);
    //CloseHandle(hToken);
    if (!result)
        return false;
    return elevation.TokenIsElevated != 0;
}
static bool elevateMode(bool e) {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken))
        return false;
    myHandle hToken(rawToken);
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        //CloseHandle(hToken);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    if(e)
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    else
        tp.Privileges[0].Attributes = 0;
    AdjustTokenPrivileges(hToken, false, &tp, sizeof(tp), nullptr, nullptr);
    //CloseHandle(hToken);
    if (GetLastError() == ERROR_SUCCESS) {
        priv_mode = e;
        return true;
    }
    return false;
}
static bool suspendProcess(DWORD pid) {
    myHandle h(OpenProcess(PROCESS_SUSPEND_RESUME, false, pid));
    if (!h) { std::cout << RED "  [ERR] Cannot open PID " << pid << RST "\n"; return false; }
    NTSTATUS st = fnSuspend ? fnSuspend(h) : STATUS_NOT_IMPLEMENTED;
    //CloseHandle(h);
    if (st == 0) { std::cout << GRN "  [OK] Suspended PID " << pid << RST "\n"; return true; }
    std::cout << RED "  [ERR] NtSuspendProcess failed (NTSTATUS 0x" << std::hex << st << std::dec << ")\n" RST;
    return false;
}
static bool resumeProcess(DWORD pid) {
    myHandle h(OpenProcess(PROCESS_SUSPEND_RESUME, false, pid));
    if (!h) { std::cout << RED "  [ERR] Cannot open PID " << pid << RST "\n"; return false; }
    NTSTATUS st = fnResume ? fnResume(h) : STATUS_NOT_IMPLEMENTED;
    //CloseHandle(h);
    if (st == 0) { std::cout << GRN "  [OK] Resumed PID " << pid << RST "\n"; return true; }
    std::cout << RED "  [ERR] NtResumeProcess failed (NTSTATUS 0x" << std::hex << st << std::dec << ")\n" RST;
    return false;
}
static void resumeAfter(DWORD pid, int secs) {
    std::cout << YLW "  [scheduled] Resume PID " << pid << " in " << secs << "s" RST "\n";
    std::thread([pid, secs]() {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        myHandle h(OpenProcess(PROCESS_SUSPEND_RESUME, false, pid));
        std::string msg;
        if (h && fnResume && fnResume(h) == 0)
            msg = std::string("\n") + GRN + "  [timer] Resumed PID " + std::to_string(pid) + RST + "\n> ";
        else
            msg = std::string("\n") + RED + "  [timer] Failed to resume PID " + std::to_string(pid) + RST + "\n> ";
        //if (h) CloseHandle(h);
        std::cout << msg; std::cout.flush();
        }).detach();
}
static bool killProcess(DWORD pid, UINT exitCode = 1) {
    myHandle h(OpenProcess(PROCESS_TERMINATE, false, pid));
    if (!h) { std::cout << RED "  [ERR] Cannot open PID " << pid << RST "\n"; return false; }
    bool ok = TerminateProcess(h, exitCode) != 0;
    //CloseHandle(h);
    if (ok) std::cout << GRN "  [OK] Terminated PID " << pid << RST "\n";
    else    std::cout << RED "  [ERR] TerminateProcess failed: " << GetLastError() << RST "\n";
    return ok;
}
static void printHeader() {
    std::cout << BOLD CYN
        << std::left
        << std::setw(7) << "PID"
        << std::setw(24) << "NAME"
        << std::setw(22) << "OWNER"
        << std::setw(9) << "CPU%"
        << std::setw(12) << "RAM(MB)"
        << std::setw(13) << "PRIV(MB)"
        << std::setw(10) << "PRIORITY"
        << RST "\n"
        << std::string(97, '-') << "\n";
}
static void printProc(const ProcessInfo& p) {
    std::string shortOwner = p.owner;
    auto bs = shortOwner.find('\\');
    if (bs != std::string::npos) shortOwner = shortOwner.substr(bs + 1);
    std::cout << std::left
        << std::setw(7) << p.pid
        << std::setw(24) << p.name.substr(0, 23)
        << std::setw(22) << shortOwner.substr(0, 21)
        << std::setw(9) << std::fixed << std::setprecision(1) << p.cpuPct
        << std::setw(12) << p.workingSetKB / 1024
        << std::setw(13) << p.virtSizeKB / 1024
        << std::setw(10) << p.priorityName
        << "\n";
}
static void printMap(DWORD pid) {
    auto regions = getMemMap(pid);
    if (regions.empty()) {
        std::cout << RED "  Cannot read map for PID " << pid
            << "  (run as Administrator)\n" RST;
        return;
    }
    std::cout << BOLD YLW "\n  Memory map — PID " << pid
        << "  (" << regions.size() << " regions)\n" RST;
    std::cout << BOLD
        << std::left
        << std::setw(20) << "  BASE ADDRESS"
        << std::setw(14) << "SIZE"
        << std::setw(10) << "STATE"
        << std::setw(10) << "TYPE"
        << std::setw(12) << "PROTECT"
        << "MAPPING\n" RST
        << "  " << std::string(82, '-') << "\n";
    for (auto& r : regions) {
        // colour protect flags
        std::string prot = r.protect;
        std::string protCol;
        if (!prot.empty()) {
            bool hasR = prot.find('r') != std::string::npos;
            bool hasW = prot.find('w') != std::string::npos;
            bool hasX = prot.find('x') != std::string::npos;
            if (hasX) protCol = YLW + prot + RST;
            else if (hasW) protCol = GRN + prot + RST;
            else if (hasR) protCol = CYN + prot + RST;
            else           protCol = DIM + prot + RST;
        }
        std::ostringstream addrStr;
        addrStr << "0x" << std::hex << std::uppercase
            << std::setw(16) << std::setfill('0') << r.baseAddr;

        std::ostringstream sizeStr;
        if (r.regionSize >= 1024 * 1024)
            sizeStr << (r.regionSize / 1024 / 1024) << " MB";
        else if (r.regionSize >= 1024)
            sizeStr << (r.regionSize / 1024) << " KB";
        else
            sizeStr << r.regionSize << " B";
        std::string stateCol;
        if (r.state == "Commit")  stateCol = GRN + r.state + RST;
        else if (r.state == "Free") stateCol = DIM + r.state + RST;
        else                      stateCol = YLW + r.state + RST;

        std::cout << "  "
            << std::left << std::setfill(' ')
            << std::setw(20) << addrStr.str()
            << std::setw(14) << sizeStr.str()
            << std::setw(17) << stateCol      // wider: ANSI codes
            << std::setw(10) << r.type
            << std::setw(19) << (protCol.empty() ? std::string("") : protCol)
            << r.mappedFile << "\n";
    }
    std::cout << "\n";
}
static void clearScreen(int lines) {
    for (int i = 0; i < lines; ++i) {
        std::cout << "\033[1A";  // move cursor up 1 line
        std::cout << "\033[2K";  // clear entire line
    }
}
static void clearScreen() {
    system("cls");
    std::cout << BOLD WHT
        "Custom Windows Task Manager.     \n"
        "Developed by Jagannath Srivatsa.     \n"
        "Run as Administrator for full access    \n"
        "type 'help' for available commands      \n"
        "For queries/bugs --> Write to jpsrivatsa@gmail.com     \n"
        "\n" RST;
    std::cout << RED "This is non risk aware manager! Be careful with SYSTEM processes while running in previlage mode!\n";
}
static void restartProcess(DWORD pid) {
    myHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION| PROCESS_TERMINATE, false, pid));
    if (!h) std::cout << RED "Faild to restart Process: " << GetLastError() << RST "\n";
    wchar_t exePath[MAX_PATH] = {};
    DWORD exeSz = MAX_PATH;
    QueryFullProcessImageNameW(h, 0, exePath, &exeSz);
    bool ok = TerminateProcess(h, 1) != 0;
    if (ok) {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        CreateProcessW(exePath, nullptr, nullptr, nullptr, false, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
        DWORD newPID = GetProcessId(pi.hProcess);
        std::cout << BOLD GRN "Process " << pid << " Restarted with PID : " << newPID << RST "\n";
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else {
        std::cout << RED "Faild to restart Process: " << GetLastError() << RST "\n";
    }
}
static void printGpu() {
    auto gpus = queryGpus();
    std::cout << BOLD YLW "\n  GPU / Display Adapters (DXGI)\n" RST;
    if (gpus.empty()) { std::cout << YLW "  No adapters found.\n" RST; return; }
    for (auto& g : gpus) {
        std::cout << BOLD GRN "  " << g.name << RST "\n"
            << "    Dedicated VRAM : " CYN << g.dedicatedVRAM_MB << " MB" RST "\n"
            << "    Shared RAM     : " CYN << g.sharedRAM_MB << " MB" RST "\n";
    }
    // GPU utilisation % requires PDH counter "\GPU Engine(*)\Utilization Percentage"
    // (Windows 10 1903+). Query it here if available.
    PDH_HQUERY q; PDH_HCOUNTER c;
    if (PdhOpenQuery(nullptr, 0, &q) == ERROR_SUCCESS) {
        if (PdhAddCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) == ERROR_SUCCESS) {
            PdhCollectQueryData(q);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            PdhCollectQueryData(q);
            PDH_FMT_COUNTERVALUE val{};
            if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS)
                std::cout << "    GPU Util (PDH)  : " CYN << std::fixed
                << std::setprecision(1) << val.doubleValue << "%" RST "\n";
        }
        PdhCloseQuery(q);
    }
    std::cout << "\n";
}
static void printHelp() {
    std::cout << BOLD CYN "\n  Commands:\n" RST
        << "  " YLW "list"                    RST "                    refresh process list\n"
        << "  " YLW "info   <pid>"            RST "            detailed process info\n"
        << "  " YLW "map    <pid>"            RST "            virtual memory map + addresses\n"
        << "  " YLW "pause  <pid>"            RST "            suspend all threads (freeze)\n"
        << "  " YLW "restart <pid>"          RST "           Restart the process\n"
        << "  " YLW "resume <pid>"            RST "            resume all threads\n"
        << "  " YLW "resume <pid> <secs>"     RST "     resume after N seconds\n"
        << "  " YLW "kill   <pid>"            RST "            TerminateProcess\n"
        << "  " YLW "kill   <pid> <exitcode>" RST "  custom exit code\n"
        << "  " YLW "gpu"                     RST "                     GPU info (DXGI + PDH)\n"
        << "  " YLW "help"                    RST "                    show this help\n"
        << "  " YLW "priv"                    RST "                    Run in Previlage mode*\n"
        << "  " YLW "unpriv"                  RST "                  Run in user mode\n"
        << "  " YLW "clear"                  RST "                   Clear Screen output\n"
        << "  " YLW "quit / exit"             RST "             exit\n"
        << "  " RED "*Caution: This is non risk aware manager! Be careful with SYSTEM processes while running in previlage mode!" RST "\n\n";
}
int main() {
    enableAnsi();
    loadNtFuncs();
    clearScreen();
    std::cout << DIM "  Sampling CPU baseline (1 second)...\n" RST;
    enumProcesses(); // first pass — seeds CPU delta
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << GRN "  Ready.\n\n" RST;
    std::string line;
    while (true) {
        if (priv_mode)
            std::cout << BOLD WHT "(priv)> " RST;
        else
            std::cout << BOLD WHT "> " RST;
        if (!std::getline(std::cin, line)) break;
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;
        if (cmd.empty()) continue;

        if (cmd == "list") {
            auto procs = enumProcesses();
            printHeader();
            for (auto& p : procs) printProc(p);
            std::cout << DIM "  " << procs.size() << " processes\n" RST;
        }
        else if (cmd == "priv")
        {
            if (!isAdmin()) {
                std::cout << BOLD RED "Run as Administrator to use this feature\n" RST; continue;
            }
            if (priv_mode)
                std::cout << BOLD WHT "Already in Previlaged Mode\n" RST;
            else
                elevateMode(true);
        }
        else if (cmd == "unpriv")
        {
            if (!isAdmin()) {
                std::cout << BOLD RED "Run as Administrator to use this feature\n" RST; continue;
            }
            if (!priv_mode)
                std::cout << BOLD WHT "Already in Unprevilaged Mode\n" RST;
            else
                elevateMode(false);
        }
        else if (cmd == "info") {
            const int infolines = 12;
            DWORD pid = 0; ss >> pid;
            DWORD exitCode = 0;
            myHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, false, pid));
            if (!h) { std::cout << RED "  PID not found or access denied\n" RST; continue; }
            while (true) {
                PROCESS_MEMORY_COUNTERS_EX pmc{ sizeof(pmc) };
                GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
                wchar_t exePath[MAX_PATH] = {};
                DWORD exeSz = MAX_PATH;
                QueryFullProcessImageNameW(h, 0, exePath, &exeSz);
                DWORD handles = 0;
                GetProcessHandleCount(h, &handles);
                FILETIME ct, et, kt, ut;
                GetProcessTimes(h, &ct, &et, &kt, &ut);
                SYSTEMTIME stCreate; FileTimeToSystemTime(&ct, &stCreate);
                std::cout << BOLD CYN "\n  PID " << pid << "\n" RST
                    << "  Executable     : " << ws2s(exePath) << "\n"
                    << "  Owner          : " << getOwner(pid) << "\n"
                    << "  CPU            : " << getCpuPct(pid) << "\n"
                    << "  Priority       : " << priorityStr(GetPriorityClass(h)) << "\n"
                    << "  Handle count   : " << handles << "\n"
                    << "  Working Set    : " << pmc.WorkingSetSize / 1024 / 1024 << " MB\n"
                    << "  Private Commit : " << pmc.PrivateUsage / 1024 / 1024 << " MB\n"
                    << "  Page Faults    : " << pmc.PageFaultCount << "\n"
                    << "  Created        : "
                    << stCreate.wYear << "-"
                    << std::setw(2) << std::setfill('0') << stCreate.wMonth << "-"
                    << std::setw(2) << std::setfill('0') << stCreate.wDay << " "
                    << std::setw(2) << std::setfill('0') << stCreate.wHour << ":"
                    << std::setw(2) << std::setfill('0') << stCreate.wMinute << "\n"
                    << " Enter 'q' to exit \n";
                if (_kbhit()) {
                    char c = _getch();
                    if (c == 'q') break;
                }
                Sleep(1000);
                clearScreen(infolines);
                if (ws2s(exePath).length() == 0)
                {
                    std::cout << RED "  Program Terminated!\n" RST; break;
                }
            }
            //CloseHandle(h);
        }
        else if (cmd == "map") {
            DWORD pid = 0; ss >> pid;
            printMap(pid);
        }
        else if (cmd == "pause") {
            DWORD pid = 0; ss >> pid;
            suspendProcess(pid);
        }
        else if (cmd == "resume") {
            DWORD pid = 0; ss >> pid;
            int secs = 0;
            if ((ss >> secs) && secs > 0) resumeAfter(pid, secs);
            else                           resumeProcess(pid);
        }
        else if (cmd == "kill") {
            DWORD pid = 0; ss >> pid;
            UINT  ec = 1; ss >> ec;
            killProcess(pid, ec);
        }
        else if (cmd == "restart") {
            DWORD pid = 0; ss >> pid;
            restartProcess(pid);
        }
        else if (cmd == "gpu") { printGpu(); }
        else if (cmd == "help") { printHelp(); }
        else if (cmd == "clear") clearScreen();
        else if (cmd == "quit" || cmd == "exit") {
            std::cout << CYN "  Bye!\n" RST; break;
        }
        else {
            std::cout << RED "  Unknown command '" << cmd << "'. Type 'help'.\n" RST;
        }
    }
    return 0;
}