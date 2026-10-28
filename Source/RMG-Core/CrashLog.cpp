/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2026 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "CrashLog.hpp"

#include "Directories.hpp"
#include "Version.hpp"
#include "Library.hpp"
#include "Error.hpp"

#include <filesystem>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <array>

#ifdef _WIN32
#include <Windows.h>
#include <Dbghelp.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#endif // _WIN32

//
// Local Variables
//

// the crash handlers run while the process is in a broken state,
// so everything they need is resolved up-front into fixed buffers
// and only async-signal-safe functions are used when writing
static constexpr int    l_MaxMessages    = 200;
static constexpr size_t l_MaxMessageSize = 512;
static constexpr size_t l_MaxPathSize    = 4096;

static char l_CrashLogDirectory[l_MaxPathSize] = {0};
static char l_CrashLogAllFile[l_MaxPathSize]   = {0};
static char l_Version[64]                      = {0};
static char l_Rom[l_MaxPathSize]               = {0};

static std::mutex l_MessagesMutex;
static std::array<std::array<char, l_MaxMessageSize>, l_MaxMessages> l_Messages;
static int  l_MessagesWritePos = 0;
static int  l_MessagesCount    = 0;
static bool l_Installed        = false;

//
// Local Functions
//

static void copy_to_buffer(char* dst, size_t dstSize, const std::string& src)
{
    size_t len = src.size();
    if (len >= dstSize)
    {
        len = dstSize - 1;
    }
    std::memcpy(dst, src.data(), len);
    dst[len] = '\0';
}

#ifdef _WIN32
typedef HANDLE crash_fd;
static const crash_fd INVALID_CRASH_FD = INVALID_HANDLE_VALUE;

static crash_fd crash_open(const char* path, bool append)
{
    HANDLE handle = CreateFileA(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle;
}

static void crash_write(crash_fd fd, const char* str, size_t len)
{
    DWORD written = 0;
    if (fd != INVALID_CRASH_FD)
    {
        WriteFile(fd, str, (DWORD)len, &written, nullptr);
    }
}

static void crash_close(crash_fd fd)
{
    if (fd != INVALID_CRASH_FD)
    {
        FlushFileBuffers(fd);
        CloseHandle(fd);
    }
}
#else
typedef int crash_fd;
static const crash_fd INVALID_CRASH_FD = -1;

static crash_fd crash_open(const char* path, bool append)
{
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    flags |= append ? O_APPEND : O_TRUNC;
    return open(path, flags, 0644);
}

static void crash_write(crash_fd fd, const char* str, size_t len)
{
    if (fd == INVALID_CRASH_FD)
    {
        return;
    }

    while (len > 0)
    {
        ssize_t ret = write(fd, str, len);
        if (ret <= 0)
        {
            return;
        }
        str += ret;
        len -= (size_t)ret;
    }
}

static void crash_close(crash_fd fd)
{
    if (fd != INVALID_CRASH_FD)
    {
        close(fd);
    }
}
#endif // _WIN32

// writes to both the per-crash file and the file that collects every crash
struct crash_output
{
    crash_fd single;
    crash_fd all;
};

static void crash_puts(crash_output& out, const char* str)
{
    size_t len = std::strlen(str);
    crash_write(out.single, str, len);
    crash_write(out.all, str, len);
}

static void crash_printf(crash_output& out, const char* fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (len > 0)
    {
        if (len >= (int)sizeof(buffer))
        {
            len = (int)sizeof(buffer) - 1;
        }
        crash_write(out.single, buffer, (size_t)len);
        crash_write(out.all, buffer, (size_t)len);
    }
}

static void crash_write_time(char* buffer, size_t bufferSize, bool forFilename)
{
    std::time_t now = std::time(nullptr);
    std::tm tm = {};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif // _WIN32
    std::strftime(buffer, bufferSize, forFilename ? "%Y-%m-%d_%H-%M-%S" : "%Y-%m-%d %H:%M:%S", &tm);
}

static void crash_write_header(crash_output& out, const char* reason)
{
    char timeBuffer[64] = {0};
    crash_write_time(timeBuffer, sizeof(timeBuffer), false);

    crash_puts(out, "================================================================================\n");
    crash_printf(out, "RMG crash at %s\n", timeBuffer);
    crash_printf(out, "Version: %s\n", l_Version);
    crash_printf(out, "ROM: %s\n", l_Rom[0] != '\0' ? l_Rom : "(none)");
    crash_printf(out, "Reason: %s\n", reason);
}

static void crash_write_messages(crash_output& out)
{
    // the mutex is intentionally not locked here,
    // we're crashing and might be holding it already
    crash_puts(out, "\nRecent log messages (oldest first):\n");
    int count = l_MessagesCount;
    int start = (l_MessagesWritePos - count + l_MaxMessages) % l_MaxMessages;
    for (int i = 0; i < count; i++)
    {
        const char* message = l_Messages[(start + i) % l_MaxMessages].data();
        crash_puts(out, "  ");
        crash_puts(out, message);
        crash_puts(out, "\n");
    }
    crash_puts(out, "================================================================================\n\n");
}

static crash_output crash_open_output(void)
{
    char timeBuffer[64] = {0};
    char path[l_MaxPathSize] = {0};
    crash_write_time(timeBuffer, sizeof(timeBuffer), true);
    snprintf(path, sizeof(path), "%s%ccrash_%s.txt", l_CrashLogDirectory,
#ifdef _WIN32
             '\\',
#else
             '/',
#endif // _WIN32
             timeBuffer);

    crash_output out;
    out.single = crash_open(path, false);
    out.all    = crash_open(l_CrashLogAllFile, true);
    return out;
}

static void crash_close_output(crash_output& out)
{
    crash_close(out.single);
    crash_close(out.all);
}

#ifdef _WIN32
typedef BOOL (WINAPI* ptr_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                             PMINIDUMP_EXCEPTION_INFORMATION,
                                             PMINIDUMP_USER_STREAM_INFORMATION,
                                             PMINIDUMP_CALLBACK_INFORMATION);
typedef BOOL (WINAPI* ptr_SymInitialize)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI* ptr_SymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL (WINAPI* ptr_SymGetLineFromAddr64)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
typedef DWORD (WINAPI* ptr_SymSetOptions)(DWORD);

static const char* exception_code_name(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        default:                                 return "UNKNOWN_EXCEPTION";
    }
}

static void crash_write_stack(crash_output& out, HMODULE dbgHelpModule, _EXCEPTION_POINTERS* exceptionInfo)
{
    crash_puts(out, "\nStack trace:\n");

    void* frames[64] = {0};
    USHORT frameCount = CaptureStackBackTrace(0, 64, frames, nullptr);

    ptr_SymInitialize symInitialize = nullptr;
    ptr_SymFromAddr symFromAddr = nullptr;
    ptr_SymGetLineFromAddr64 symGetLineFromAddr64 = nullptr;
    ptr_SymSetOptions symSetOptions = nullptr;
    HANDLE process = GetCurrentProcess();
    if (dbgHelpModule != nullptr)
    {
        symInitialize = (ptr_SymInitialize)GetProcAddress(dbgHelpModule, "SymInitialize");
        symFromAddr = (ptr_SymFromAddr)GetProcAddress(dbgHelpModule, "SymFromAddr");
        symGetLineFromAddr64 = (ptr_SymGetLineFromAddr64)GetProcAddress(dbgHelpModule, "SymGetLineFromAddr64");
        symSetOptions = (ptr_SymSetOptions)GetProcAddress(dbgHelpModule, "SymSetOptions");
    }

    bool haveSymbols = false;
    if (symInitialize != nullptr && symFromAddr != nullptr)
    {
        if (symSetOptions != nullptr)
        {
            symSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        }
        haveSymbols = symInitialize(process, nullptr, TRUE);
    }

    // start with the faulting address, CaptureStackBackTrace
    // only sees the handler's own frames above it
    void* faultAddress = exceptionInfo->ExceptionRecord->ExceptionAddress;

    for (USHORT i = 0; i <= frameCount; i++)
    {
        void* address = (i == 0) ? faultAddress : frames[i - 1];
        if (address == nullptr)
        {
            continue;
        }

        char moduleName[MAX_PATH] = {0};
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)address, &module))
        {
            GetModuleFileNameA(module, moduleName, MAX_PATH);
        }

        const char* baseName = std::strrchr(moduleName, '\\');
        baseName = (baseName != nullptr) ? (baseName + 1) : moduleName;
        uintptr_t offset = (module != nullptr) ? ((uintptr_t)address - (uintptr_t)module) : (uintptr_t)address;

        char symbolBuffer[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        const char* symbolName = "?";
        if (haveSymbols && symFromAddr(process, (DWORD64)address, &displacement, symbol))
        {
            symbolName = symbol->Name;
        }

        IMAGEHLP_LINE64 line = {0};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisplacement = 0;
        if (haveSymbols && symGetLineFromAddr64 != nullptr &&
            symGetLineFromAddr64(process, (DWORD64)address, &lineDisplacement, &line))
        {
            crash_printf(out, "  #%-2u %p %s+0x%llx %s (%s:%lu)\n", (unsigned)i, address, baseName[0] ? baseName : "?",
                         (unsigned long long)offset, symbolName, line.FileName, (unsigned long)line.LineNumber);
        }
        else
        {
            crash_printf(out, "  #%-2u %p %s+0x%llx %s\n", (unsigned)i, address, baseName[0] ? baseName : "?",
                         (unsigned long long)offset, symbolName);
        }
    }
}

static LONG WINAPI exception_handler(_EXCEPTION_POINTERS* exceptionInfo)
{
    // only handle the first crash,
    // a crash inside the handler must not recurse
    static volatile LONG handling = 0;
    if (InterlockedCompareExchange(&handling, 1, 0) != 0)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    HMODULE dbgHelpModule = LoadLibraryA("Dbghelp.dll");

    char reason[256] = {0};
    DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && exceptionInfo->ExceptionRecord->NumberParameters >= 2)
    {
        snprintf(reason, sizeof(reason), "%s (0x%08lX) %s address %p, instruction at %p",
                 exception_code_name(code), (unsigned long)code,
                 exceptionInfo->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" :
                 exceptionInfo->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : "executing",
                 (void*)exceptionInfo->ExceptionRecord->ExceptionInformation[1],
                 exceptionInfo->ExceptionRecord->ExceptionAddress);
    }
    else
    {
        snprintf(reason, sizeof(reason), "%s (0x%08lX), instruction at %p",
                 exception_code_name(code), (unsigned long)code,
                 exceptionInfo->ExceptionRecord->ExceptionAddress);
    }

    crash_output out = crash_open_output();
    crash_write_header(out, reason);
    crash_printf(out, "Thread: %lu\n", (unsigned long)GetCurrentThreadId());
    crash_write_stack(out, dbgHelpModule, exceptionInfo);

    // write a minidump next to the crash log
    // to aid with debugging the crash
    ptr_MiniDumpWriteDump miniDumpWriteDump = nullptr;
    if (dbgHelpModule != nullptr)
    {
        miniDumpWriteDump = (ptr_MiniDumpWriteDump)GetProcAddress(dbgHelpModule, "MiniDumpWriteDump");
    }

    if (miniDumpWriteDump != nullptr)
    {
        char timeBuffer[64] = {0};
        char dumpPath[l_MaxPathSize] = {0};
        crash_write_time(timeBuffer, sizeof(timeBuffer), true);
        snprintf(dumpPath, sizeof(dumpPath), "%s\\crash_%s.dmp", l_CrashLogDirectory, timeBuffer);

        HANDLE dumpFile = CreateFileA(dumpPath, GENERIC_WRITE, FILE_SHARE_WRITE,
                                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dumpFile != INVALID_HANDLE_VALUE)
        {
            _MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
            dumpInfo.ThreadId = GetCurrentThreadId();
            dumpInfo.ExceptionPointers = exceptionInfo;
            dumpInfo.ClientPointers = FALSE;

            BOOL ret = miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                         dumpFile, MiniDumpNormal, &dumpInfo,
                                         nullptr, nullptr);
            CloseHandle(dumpFile);
            crash_printf(out, "\nMinidump: %s\n", ret ? dumpPath : "failed to write");
        }
    }
    else
    {
        crash_puts(out, "\nMinidump: Dbghelp.dll not available\n");
    }

    crash_write_messages(out);
    crash_close_output(out);

    char message[l_MaxPathSize + 128] = {0};
    snprintf(message, sizeof(message), "RMG has crashed.\n\nA crash log has been written to:\n%s", l_CrashLogDirectory);
    MessageBoxA(nullptr, message, "RMG Crash Handler", MB_OK | MB_ICONERROR);

    // we've saved the crash log,
    // now we can crash safely
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static const char* signal_name(int sig)
{
    switch (sig)
    {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGFPE:  return "SIGFPE (arithmetic exception)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGTRAP: return "SIGTRAP (trap)";
        default:      return "unknown signal";
    }
}

static void crash_write_stack(crash_output& out)
{
    crash_puts(out, "\nStack trace:\n");

    void* frames[64];
    int frameCount = backtrace(frames, 64);
    for (int i = 0; i < frameCount; i++)
    {
        Dl_info info;
        std::memset(&info, 0, sizeof(info));
        if (dladdr(frames[i], &info) != 0 && info.dli_fname != nullptr)
        {
            const char* baseName = std::strrchr(info.dli_fname, '/');
            baseName = (baseName != nullptr) ? (baseName + 1) : info.dli_fname;
            uintptr_t offset = (uintptr_t)frames[i] - (uintptr_t)info.dli_fbase;
            crash_printf(out, "  #%-2d %p %s+0x%llx %s\n", i, frames[i], baseName,
                         (unsigned long long)offset, info.dli_sname != nullptr ? info.dli_sname : "?");
        }
        else
        {
            crash_printf(out, "  #%-2d %p ?\n", i, frames[i]);
        }
    }
}

static void signal_handler(int sig, siginfo_t* info, void*)
{
    // only handle the first crash,
    // a crash inside the handler must not recurse
    static volatile sig_atomic_t handling = 0;
    if (handling)
    {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    handling = 1;

    char reason[256] = {0};
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE)
    {
        snprintf(reason, sizeof(reason), "%s at address %p (code %d)", signal_name(sig), info->si_addr, info->si_code);
    }
    else
    {
        snprintf(reason, sizeof(reason), "%s", signal_name(sig));
    }

    crash_output out = crash_open_output();
    crash_write_header(out, reason);
    crash_write_stack(out);
    crash_write_messages(out);
    crash_close_output(out);

    const char notice[] = "RMG has crashed, a crash log has been written to: ";
    crash_write(STDERR_FILENO, notice, sizeof(notice) - 1);
    crash_write(STDERR_FILENO, l_CrashLogDirectory, std::strlen(l_CrashLogDirectory));
    crash_write(STDERR_FILENO, "\n", 1);

    // we've saved the crash log,
    // now we can crash safely
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif // _WIN32

//
// Exported Functions
//

CORE_EXPORT std::filesystem::path CoreGetCrashLogDirectory(void)
{
    std::filesystem::path directory;
#ifdef PORTABLE_INSTALL
    if (CoreGetPortableDirectoryMode())
    {
        directory = "CrashLogs";
    }
    else
#endif // PORTABLE_INSTALL
    {
        directory = CoreGetUserDataDirectory();
        directory += "/CrashLogs";
    }
    return directory.make_preferred();
}

CORE_EXPORT std::filesystem::path CoreGetCrashLogFile(void)
{
    std::filesystem::path file = CoreGetCrashLogDirectory();
    file += "/crashlogs_all.txt";
    return file.make_preferred();
}

CORE_EXPORT bool CoreSetupCrashLog(void)
{
    std::string error;
    std::error_code errorCode;

    if (l_Installed)
    {
        return true;
    }

    std::filesystem::path directory = CoreGetCrashLogDirectory();
    if (!std::filesystem::is_directory(directory) &&
        !std::filesystem::create_directories(directory, errorCode))
    {
        error = "CoreSetupCrashLog Failed: cannot create the '";
        error += directory.string();
        error += "' directory: ";
        error += errorCode.message();
        CoreSetError(error);
        return false;
    }

    // resolve everything to absolute paths now,
    // the working directory might change later on
    directory = std::filesystem::absolute(directory, errorCode);
    copy_to_buffer(l_CrashLogDirectory, sizeof(l_CrashLogDirectory), directory.string());
    copy_to_buffer(l_CrashLogAllFile, sizeof(l_CrashLogAllFile), (directory / "crashlogs_all.txt").make_preferred().string());
    copy_to_buffer(l_Version, sizeof(l_Version), CoreGetVersion());

#ifdef _WIN32
    SetUnhandledExceptionFilter(exception_handler);
#else
    // give the handler its own stack,
    // so a stack overflow can still be logged
    static std::array<char, 256 * 1024> alternateStack;
    stack_t stack;
    stack.ss_sp = alternateStack.data();
    stack.ss_size = alternateStack.size();
    stack.ss_flags = 0;
    sigaltstack(&stack, nullptr);

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = signal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);

    const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
    for (int sig : signals)
    {
        sigaction(sig, &action, nullptr);
    }
#endif // _WIN32

    l_Installed = true;
    return true;
}

CORE_EXPORT void CoreCrashLogAddMessage(const std::string& message)
{
    const std::lock_guard<std::mutex> guard(l_MessagesMutex);

    copy_to_buffer(l_Messages[l_MessagesWritePos].data(), l_MaxMessageSize, message);
    l_MessagesWritePos = (l_MessagesWritePos + 1) % l_MaxMessages;
    if (l_MessagesCount < l_MaxMessages)
    {
        l_MessagesCount++;
    }
}

CORE_EXPORT void CoreCrashLogSetRom(const std::string& rom)
{
    copy_to_buffer(l_Rom, sizeof(l_Rom), rom);
}
