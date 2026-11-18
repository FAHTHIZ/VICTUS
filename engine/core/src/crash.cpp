// SPDX-License-Identifier: Apache-2.0
// Crash reports and diagnostic sessions (ADR-0072). See victus/core/crash.hpp.
#include "victus/core/crash.hpp"
#include "victus/core/assert.hpp"
#include "victus/core/build_info.hpp"
#include "victus/core/log.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <share.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#if (defined(__GLIBC__) || defined(__APPLE__)) && !defined(__ANDROID__)
#include <execinfo.h>
#define VICTUS_HAVE_EXECINFO 1
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <elf.h>
#include <link.h>
#define VICTUS_HAVE_BUILD_ID 1
#endif
#endif

namespace victus::crash {

namespace {

namespace sfs = std::filesystem;

// ---- Fixed-size state used by the fatal path (no allocation after install) ----------------
constexpr std::size_t kPathMax = 1024;
constexpr std::size_t kCrumbs = 64;
constexpr std::size_t kCrumbLen = 256;
constexpr int kMaxFrames = 64;

char g_crash_dir[kPathMax] = {0};
char g_app[64] = "victus";
char g_header[2048] = {0}; // version/build/platform lines, formatted at install
std::time_t g_start = 0;
std::atomic<bool> g_in_fatal{false};
char g_report[64 * 1024];
char g_last_path[kPathMax] = {0};
char g_exe[kPathMax] = {0}; // absolute executable path (dladdr may report argv[0], e.g. "./game")

char g_crumbs[kCrumbs][kCrumbLen];
std::atomic<std::uint64_t> g_crumb_next{0};

#if defined(VICTUS_HAVE_BUILD_ID)
struct ModuleId {
    std::uintptr_t base;
    char id[41];
};
constexpr std::size_t kMaxModules = 128;
ModuleId g_modules[kMaxModules];
std::size_t g_module_count = 0;

int collect_build_id(dl_phdr_info* info, std::size_t, void*) {
    if (g_module_count >= kMaxModules) return 1;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        auto* p = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        const unsigned char* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto* nh = reinterpret_cast<const ElfW(Nhdr)*>(p);
            const unsigned char* name = p + sizeof(ElfW(Nhdr));
            const unsigned char* desc = name + ((nh->n_namesz + 3u) & ~3u);
            if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && std::memcmp(name, "GNU", 4) == 0 && desc + nh->n_descsz <= end) {
                ModuleId& m = g_modules[g_module_count++];
                m.base = info->dlpi_addr;
                const std::size_t n = std::min<std::size_t>(nh->n_descsz, 20);
                for (std::size_t b = 0; b < n; ++b) std::snprintf(m.id + b * 2, 3, "%02x", desc[b]);
                m.id[n * 2] = 0;
                return 0;
            }
            p = desc + ((nh->n_descsz + 3u) & ~3u);
        }
    }
    return 0;
}
#endif

// Appends to a fixed buffer without allocating.
struct Out {
    char* data;
    std::size_t cap, n{0};
    void put(const char* s) {
        while (s && *s && n + 1 < cap) data[n++] = *s++;
        data[n] = 0;
    }
    void put(const char* s, std::size_t len) {
        for (std::size_t i = 0; i < len && s[i] && n + 1 < cap; ++i) data[n++] = s[i];
        data[n] = 0;
    }
    void dec(std::uint64_t v) {
        char tmp[24];
        int i = 0;
        do tmp[i++] = static_cast<char>('0' + v % 10);
        while ((v /= 10) && i < 23);
        while (i) {
            char c[2] = {tmp[--i], 0};
            put(c);
        }
    }
    void hex(std::uintptr_t v) {
        put("0x");
        char tmp[2 * sizeof v + 1];
        int i = 0;
        do {
            const unsigned d = static_cast<unsigned>(v & 0xfu);
            tmp[i++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        } while ((v >>= 4) && i < static_cast<int>(sizeof tmp) - 1);
        while (i) {
            char c[2] = {tmp[--i], 0};
            put(c);
        }
    }
};

std::uint64_t current_pid() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

void format_header() {
    std::snprintf(g_header, sizeof g_header, "version: %s\nrevision: %s\nplatform: %s %s\ncompiler: %s\nbuild: %s\n",
                  build::version(), build::git_revision(), build::platform(), build::architecture(), build::compiler(),
                  build::build_type());
}

// Frame lines for `frames`; module, offset, symbol and build id where the platform knows them.
void put_frames(Out& o, void* const* frames, int count) {
    o.put("--- backtrace ---\n");
    if (count <= 0) o.put("(no frames available on this platform)\n");
    for (int i = 0; i < count; ++i) {
        const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
        o.put("#");
        o.dec(static_cast<std::uint64_t>(i));
        o.put(" ");
        o.hex(addr);
#if defined(_WIN32)
        HMODULE mod = nullptr;
        char name[MAX_PATH] = {0};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]), &mod) &&
            GetModuleFileNameA(mod, name, MAX_PATH)) {
            o.put(" ");
            o.put(name);
            o.put("+");
            o.hex(addr - reinterpret_cast<std::uintptr_t>(mod));
        }
#elif !defined(_WIN32)
        Dl_info info{};
        if (dladdr(frames[i], &info) && info.dli_fname) {
            o.put(" ");
            o.put(info.dli_fname[0] != '/' && g_exe[0] ? g_exe : info.dli_fname);
            o.put("+");
            o.hex(addr - reinterpret_cast<std::uintptr_t>(info.dli_fbase));
            if (info.dli_sname) {
                o.put(" (");
                o.put(info.dli_sname);
                o.put(")");
            }
#if defined(VICTUS_HAVE_BUILD_ID)
            for (std::size_t m = 0; m < g_module_count; ++m)
                if (g_modules[m].base == reinterpret_cast<std::uintptr_t>(info.dli_fbase)) {
                    o.put(" [build-id ");
                    o.put(g_modules[m].id);
                    o.put("]");
                    break;
                }
#endif
        }
#endif
        o.put("\n");
    }
}

void put_crumbs(Out& o) {
    o.put("--- recent log ---\n");
    const std::uint64_t next = g_crumb_next.load(std::memory_order_acquire);
    const std::uint64_t first = next > kCrumbs ? next - kCrumbs : 0;
    if (next == 0) o.put("(no log lines recorded)\n");
    for (std::uint64_t i = first; i < next; ++i) o.put(g_crumbs[i % kCrumbs], kCrumbLen);
}

int capture_frames(void** frames, int max) {
#if defined(VICTUS_HAVE_EXECINFO)
    return backtrace(frames, max);
#elif defined(_WIN32)
    return static_cast<int>(CaptureStackBackTrace(0, static_cast<DWORD>(max), frames, nullptr));
#else
    (void)frames;
    (void)max;
    return 0;
#endif
}

// Builds the report in g_report and writes it to crashes/crash-<time>-<pid>-<app>.txt.
// Returns false if there is no directory or the file cannot be written.
bool write_fatal(const char* reason, std::uintptr_t fault_address, bool has_fault_address, void* const* frames, int count) {
    if (!g_crash_dir[0]) return false;
    const std::time_t now = std::time(nullptr);
    Out path{g_last_path, sizeof g_last_path};
    path.put(g_crash_dir);
    path.put("/crash-");
    path.dec(static_cast<std::uint64_t>(now));
    path.put("-");
    path.dec(current_pid());
    path.put("-");
    path.put(g_app);
    path.put(".txt");

    Out o{g_report, sizeof g_report};
    o.put("VICTUS crash report\nformat: 1\nreason: ");
    o.put(reason);
    o.put("\n");
    if (has_fault_address) {
        o.put("fault_address: ");
        o.hex(fault_address);
        o.put("\n");
    }
    o.put("app: ");
    o.put(g_app);
    o.put("\npid: ");
    o.dec(current_pid());
    o.put("\nunix_time: ");
    o.dec(static_cast<std::uint64_t>(now));
    o.put("\nuptime_seconds: ");
    o.dec(static_cast<std::uint64_t>(now >= g_start ? now - g_start : 0));
    o.put("\n");
    o.put(g_header);
    put_frames(o, frames, count);
    put_crumbs(o);

#if defined(_WIN32)
    std::FILE* f = nullptr;
    if (fopen_s(&f, g_last_path, "wb") != 0 || !f) return false;
    std::fwrite(g_report, 1, o.n, f);
    std::fclose(f);
#else
    const int fd = ::open(g_last_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < o.n) {
        const ssize_t w = ::write(fd, g_report + done, o.n - done);
        if (w <= 0) break;
        done += static_cast<std::size_t>(w);
    }
    ::close(fd);
#endif
    return true;
}

void announce(const char* what, bool written) {
    Out o{g_report, sizeof g_report};
    o.put("VICTUS fatal ");
    o.put(what);
    if (written) {
        o.put("; crash report written to ");
        o.put(g_last_path);
    }
    o.put("\n");
#if defined(_WIN32)
    std::fputs(g_report, stderr);
#else
    [[maybe_unused]] const ssize_t w = ::write(2, g_report, o.n);
#endif
}

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGILL: return "SIGILL (illegal instruction)";
#if !defined(_WIN32)
        case SIGBUS: return "SIGBUS (bus error)";
#endif
        default: return "fatal signal";
    }
}

#if defined(_WIN32)
extern "C" void victus_fatal_signal(int sig) {
    if (g_in_fatal.exchange(true)) std::_Exit(128 + sig);
    void* frames[kMaxFrames];
    const int n = capture_frames(frames, kMaxFrames);
    announce(signal_name(sig), write_fatal(signal_name(sig), 0, false, frames, n));
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// An uncaught C++ exception reaches the unhandled-exception filter as SEH code 0xE06D7363 (the
// filter replaces the CRT's, which would only call std::terminate without the exception). The
// record carries the thrown object and the compiler's ThrowInfo; if one of its catchable types is
// std::exception, its what() goes into the report. Layout of the MSVC EH structures (RVAs from
// the image base in ExceptionInformation[3] on 64-bit; absolute pointers on 32-bit).
constexpr DWORD kMsvcCppException = 0xE06D7363;
struct EhThrowInfo {
    std::uint32_t attributes;
    std::int32_t unwind, forward_compat, catchable_types;
};
struct EhCatchableType {
    std::uint32_t properties;
    std::int32_t type;
    std::int32_t mdisp, pdisp, vdisp;
    std::int32_t size_or_offset, copy;
};
struct EhTypeDescriptor {
    const void* vftable;
    void* spare;
    char name[1];
};
char g_cpp_reason[512];

const char* cpp_exception_reason(const EXCEPTION_RECORD& r) {
    const auto n = r.NumberParameters;
    if (n < 3 || (r.ExceptionInformation[0] & ~std::uintptr_t{3}) != 0x19930520u) return nullptr;
    const auto object = static_cast<std::uintptr_t>(r.ExceptionInformation[1]);
    const auto* info = reinterpret_cast<const EhThrowInfo*>(r.ExceptionInformation[2]);
    const std::uintptr_t base = n >= 4 ? static_cast<std::uintptr_t>(r.ExceptionInformation[3]) : 0;
    if (!object || !info) return nullptr;
    const auto at = [base](std::int32_t rva) { return base + static_cast<std::uintptr_t>(static_cast<std::uint32_t>(rva)); };
    const auto* array = reinterpret_cast<const std::int32_t*>(at(info->catchable_types));
    const std::int32_t count = array[0];
    for (std::int32_t i = 0; i < count && i < 64; ++i) {
        const auto* ct = reinterpret_cast<const EhCatchableType*>(at(array[1 + i]));
        const auto* td = reinterpret_cast<const EhTypeDescriptor*>(at(ct->type));
        if (std::strcmp(td->name, ".?AVexception@std@@") != 0 || ct->pdisp != -1) continue;
        const auto* e = reinterpret_cast<const std::exception*>(object + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(ct->mdisp)));
        std::snprintf(g_cpp_reason, sizeof g_cpp_reason, "unhandled exception: %s", e->what());
        return g_cpp_reason;
    }
    return "unhandled exception (not a std::exception)";
}

const char* safe_cpp_exception_reason(const EXCEPTION_RECORD& r) {
#if defined(_MSC_VER)
    __try {
        return cpp_exception_reason(r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
#else
    return cpp_exception_reason(r);
#endif
}

LONG WINAPI victus_unhandled_exception(EXCEPTION_POINTERS* ep) {
    if (g_in_fatal.exchange(true)) return EXCEPTION_EXECUTE_HANDLER;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    const char* cpp = code == kMsvcCppException ? safe_cpp_exception_reason(*ep->ExceptionRecord) : nullptr;
    const char* reason = cpp                                       ? cpp
                         : code == kMsvcCppException               ? "unhandled exception"
                         : code == EXCEPTION_ACCESS_VIOLATION       ? "EXCEPTION_ACCESS_VIOLATION (invalid memory access)"
                         : code == EXCEPTION_STACK_OVERFLOW       ? "EXCEPTION_STACK_OVERFLOW"
                         : code == EXCEPTION_ILLEGAL_INSTRUCTION  ? "EXCEPTION_ILLEGAL_INSTRUCTION"
                         : code == EXCEPTION_INT_DIVIDE_BY_ZERO   ? "EXCEPTION_INT_DIVIDE_BY_ZERO"
                         : code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ? "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"
                                                                  : "unhandled structured exception";
    std::uintptr_t fault = 0;
    bool has_fault = false;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        fault = static_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        has_fault = true;
    }
    void* frames[kMaxFrames];
    const int n = capture_frames(frames, kMaxFrames);
    announce(reason, write_fatal(reason, fault, has_fault, frames, n));
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
alignas(16) char g_alt_stack[64 * 1024];

extern "C" void victus_fatal_signal(int sig, siginfo_t* info, void*) {
    if (g_in_fatal.exchange(true)) {
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }
    void* frames[kMaxFrames];
    const int n = capture_frames(frames, kMaxFrames);
    const bool has_fault = sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL;
    announce(signal_name(sig),
             write_fatal(signal_name(sig), has_fault && info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0, has_fault,
                         frames, n));
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
#endif

[[noreturn]] void victus_terminate() {
    std::string what = "std::terminate";
    if (auto ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = std::string("std::terminate (unhandled exception: ") + e.what() + ")";
        } catch (...) {
            what = "std::terminate (unhandled non-standard exception)";
        }
    }
    if (!g_in_fatal.exchange(true)) {
        void* frames[kMaxFrames];
        const int n = capture_frames(frames, kMaxFrames);
        announce(what.c_str(), write_fatal(what.c_str(), 0, false, frames, n));
    }
    try {
        log::logger().flush();
    } catch (...) {
    }
    std::_Exit(3);
}

// Keeps the last kCrumbs log lines in g_crumbs for crash reports.
class CrumbSink final : public log::Sink {
public:
    void write(const log::Record& r) override {
        const std::uint64_t i = g_crumb_next.load(std::memory_order_relaxed);
        const auto t = std::chrono::system_clock::to_time_t(r.time);
        char* slot = g_crumbs[i % kCrumbs];
        const int n = std::snprintf(slot, kCrumbLen, "[%lld] [%s] [%s] %s\n", static_cast<long long>(t), log::to_string(r.level),
                                    r.category.c_str(), r.message.c_str());
        if (n < 0) slot[0] = 0;
        else if (static_cast<std::size_t>(n) >= kCrumbLen) slot[kCrumbLen - 2] = '\n'; // a cut line still ends the line
        g_crumb_next.store(i + 1, std::memory_order_release);
    }
};

// Session log: flushed per line so a crash loses nothing that was logged.
class LineFileSink final : public log::Sink {
public:
    explicit LineFileSink(const fs::Path& path) {
#if defined(_WIN32)
        // _wfopen_s denies sharing; the log must stay readable (diagnostics bundles, tests) while open.
        f_ = _wfsopen(path.wstring().c_str(), L"wb", _SH_DENYWR);
#else
        f_ = std::fopen(path.string().c_str(), "wb");
#endif
    }
    ~LineFileSink() override {
        if (f_) std::fclose(f_);
    }
    LineFileSink(const LineFileSink&) = delete;
    LineFileSink& operator=(const LineFileSink&) = delete;
    [[nodiscard]] bool ok() const noexcept { return f_ != nullptr; }
    void write(const log::Record& r) override {
        if (!f_) return;
        const auto t = std::chrono::system_clock::to_time_t(r.time);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::fprintf(f_, "%04d-%02d-%02dT%02d:%02d:%02dZ [%s] [%s] %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, log::to_string(r.level), r.category.c_str(), r.message.c_str());
        std::fflush(f_);
    }
    void flush() override {
        if (f_) std::fflush(f_);
    }

private:
    std::FILE* f_{nullptr};
};

std::string safe_app(std::string_view app) {
    std::string out;
    for (char c : app)
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_');
    if (out.empty()) out = "victus";
    if (out.size() > 48) out.resize(48);
    return out;
}

bool process_alive(std::uint64_t pid) {
    if (pid == current_pid()) return true;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

// "<prefix><time>-<pid>..." -> (time, pid); false if the name does not match.
bool parse_stamp(std::string_view name, std::string_view prefix, std::int64_t& time, std::uint64_t& pid, std::string_view& rest) {
    if (!name.starts_with(prefix)) return false;
    name.remove_prefix(prefix.size());
    std::uint64_t parts[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        std::size_t i = 0;
        while (i < name.size() && name[i] >= '0' && name[i] <= '9') parts[k] = parts[k] * 10 + static_cast<std::uint64_t>(name[i++] - '0');
        if (i == 0) return false;
        name.remove_prefix(i);
        if (k == 0) {
            if (name.empty() || name[0] != '-') return false;
            name.remove_prefix(1);
        }
    }
    time = static_cast<std::int64_t>(parts[0]);
    pid = parts[1];
    rest = name;
    return true;
}

// Deletes all but the newest `keep` files in `dir` whose names start with `prefix` and end with `ext`.
void prune(const fs::Path& dir, std::string_view prefix, std::string_view ext, std::size_t keep) {
    std::vector<std::pair<std::int64_t, fs::Path>> files;
    std::error_code ec;
    for (const auto& e : sfs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        std::int64_t t = 0;
        std::uint64_t pid = 0;
        std::string_view rest;
        if (!name.ends_with(ext) || !parse_stamp(name, prefix, t, pid, rest)) continue;
        files.emplace_back(t, e.path());
    }
    if (files.size() <= keep) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second > b.second); });
    for (std::size_t i = keep; i < files.size(); ++i) sfs::remove(files[i].second, ec);
}

} // namespace

fs::Path default_directory() {
    if (const char* env = std::getenv("VICTUS_DIAGNOSTICS_DIR"); env && *env) return fs::Path(env);
    return fs::user_data_dir() / "diagnostics";
}

void install_handlers(const fs::Path& directory, std::string_view app) {
    std::error_code ec;
    const fs::Path crashes = directory / "crashes";
    sfs::create_directories(crashes, ec);
    std::snprintf(g_crash_dir, sizeof g_crash_dir, "%s", crashes.generic_string().c_str());
    std::snprintf(g_app, sizeof g_app, "%s", safe_app(app).c_str());
    format_header();
    std::snprintf(g_exe, sizeof g_exe, "%s", fs::executable_path().string().c_str());
    if (!g_start) g_start = std::time(nullptr);
#if defined(VICTUS_HAVE_BUILD_ID)
    g_module_count = 0;
    dl_iterate_phdr(collect_build_id, nullptr);
#endif
    void* warm[2];
    (void)capture_frames(warm, 2); // loads the unwinder now, not inside a signal handler
#if defined(_WIN32)
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(sig, victus_fatal_signal);
    SetUnhandledExceptionFilter(victus_unhandled_exception);
    // No "abort() has been called" dialog or Windows Error Reporting prompt: the report is ours.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    stack_t ss{};
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof g_alt_stack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = victus_fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) sigaction(sig, &sa, nullptr);
#endif
    g_in_fatal.store(false);
    std::set_terminate(victus_terminate);
}

Result<fs::Path> write_report(std::string_view reason) {
    if (!g_crash_dir[0]) return make_error(ErrorCode::InvalidArgument, "crash handlers are not installed");
    void* frames[kMaxFrames];
    const int n = capture_frames(frames, kMaxFrames);
    const std::string r(reason);
    if (!write_fatal(r.c_str(), 0, false, frames, n))
        return make_error(ErrorCode::IoError, std::string("cannot write crash report ") + g_last_path);
    return fs::Path(g_last_path);
}

namespace {
int* volatile g_null_target = nullptr;
}

bool trigger(std::string_view kind) {
    if (kind == "segv") {
        *g_null_target = 42;
        return true;
    }
    if (kind == "abort") std::abort();
    if (kind == "terminate") throw std::runtime_error("victus crash test");
    if (kind == "assert") VICTUS_VERIFY(kind.empty(), "victus crash test");
    return false;
}

std::vector<ReportInfo> list_reports(const fs::Path& directory) {
    std::vector<ReportInfo> out;
    std::error_code ec;
    for (const auto& e : sfs::directory_iterator(directory / "crashes", ec)) {
        const std::string name = e.path().filename().string();
        ReportInfo info;
        std::uint64_t pid = 0;
        std::string_view rest;
        if (!name.ends_with(".txt") || !parse_stamp(name, "crash-", info.unix_time, pid, rest) || rest.size() < 5 || rest[0] != '-')
            continue;
        info.app = std::string(rest.substr(1, rest.size() - 5));
        info.path = e.path();
        if (std::FILE* f = std::fopen(e.path().string().c_str(), "rb")) {
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                if (std::strncmp(line, "reason: ", 8) != 0) continue;
                info.reason = line + 8;
                while (!info.reason.empty() && (info.reason.back() == '\n' || info.reason.back() == '\r')) info.reason.pop_back();
                break;
            }
            std::fclose(f);
        }
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const ReportInfo& a, const ReportInfo& b) {
        return a.unix_time > b.unix_time || (a.unix_time == b.unix_time && a.path > b.path);
    });
    return out;
}

Result<std::unique_ptr<Session>> Session::start(Options options) {
    std::unique_ptr<Session> s(new Session());
    s->dir_ = options.directory.empty() ? default_directory() : options.directory;
    const std::string app = safe_app(options.app);
    std::error_code ec;
    for (const char* sub : {"crashes", "logs", "sessions"}) {
        sfs::create_directories(s->dir_ / sub, ec);
        if (ec) return make_error(ErrorCode::IoError, "cannot create " + (s->dir_ / sub).string() + ": " + ec.message());
    }
    // Markers of earlier sessions of this app whose process is gone: they did not exit cleanly.
    for (const auto& e : sfs::directory_iterator(s->dir_ / "sessions", ec)) {
        const std::string name = e.path().filename().string();
        const std::string prefix = app + "-";
        if (!name.starts_with(prefix) || !name.ends_with(".running")) continue;
        const std::uint64_t pid = std::strtoull(name.c_str() + prefix.size(), nullptr, 10);
        if (pid && process_alive(pid)) continue;
        s->previous_unclean_ = true;
        sfs::remove(e.path(), ec);
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const std::uint64_t pid = current_pid();
    s->marker_ = s->dir_ / "sessions" / (app + "-" + std::to_string(pid) + ".running");
    if (std::FILE* f = std::fopen(s->marker_.string().c_str(), "wb")) {
        std::fprintf(f, "%lld\n", static_cast<long long>(now));
        std::fclose(f);
    }
    prune(s->dir_ / "crashes", "crash-", ".txt", options.keep_crashes);
    install_handlers(s->dir_, app);
    s->crumbs_ = std::make_shared<CrumbSink>();
    log::logger().add_sink(s->crumbs_);
    if (options.log_file) {
        s->log_path_ = s->dir_ / "logs" / (app + "-" + std::to_string(now) + "-" + std::to_string(pid) + ".log");
        auto sink = std::make_shared<LineFileSink>(s->log_path_);
        if (sink->ok()) {
            s->file_sink_ = sink;
            log::logger().add_sink(sink);
        } else {
            s->log_path_.clear();
        }
        prune(s->dir_ / "logs", app + "-", ".log", std::max<std::size_t>(options.keep_logs, 1));
    }
    VICTUS_LOG_INFO("diagnostics", "{} {} ({}) on {} {}, {} build; diagnostics in {}", app, build::version(), build::git_revision(),
                    build::platform(), build::architecture(), build::build_type(), s->dir_.string());
    if (s->previous_unclean_) VICTUS_LOG_WARN("diagnostics", "the previous {} session ended unexpectedly", app);
    return s;
}

Session::~Session() {
    std::error_code ec;
    if (!marker_.empty()) sfs::remove(marker_, ec);
    if (file_sink_) log::logger().remove_sink(file_sink_);
    if (crumbs_) log::logger().remove_sink(crumbs_);
}

} // namespace victus::crash
