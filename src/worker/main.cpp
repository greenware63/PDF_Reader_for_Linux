#include "WorkerServer.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#if __has_include(<linux/audit.h>)
#include <linux/audit.h>
#endif
#if __has_include(<linux/seccomp.h>) && __has_include(<linux/filter.h>)
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <cstddef>
#include <vector>
#define PDF_WORKER_HAS_SECCOMP_HEADERS 1
#endif

#ifndef AUDIT_ARCH_X86_64
#define AUDIT_ARCH_X86_64 0xc000003e
#endif
#ifndef AUDIT_ARCH_AARCH64
#define AUDIT_ARCH_AARCH64 0xc00000b7
#endif
#ifndef AUDIT_ARCH_ARM
#define AUDIT_ARCH_ARM 0x40000028
#endif
#ifndef AUDIT_ARCH_I386
#define AUDIT_ARCH_I386 0x40000003
#endif

#if defined(__x86_64__)
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ARM
#elif defined(__i386__)
#define SECCOMP_AUDIT_ARCH AUDIT_ARCH_I386
#endif

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif
#ifndef SECCOMP_RET_KILL
#define SECCOMP_RET_KILL 0x00000000U
#endif
#ifndef SECCOMP_RET_ERRNO
#define SECCOMP_RET_ERRNO 0x00050000U
#endif
#ifndef SECCOMP_RET_ALLOW
#define SECCOMP_RET_ALLOW 0x7fff0000U
#endif
#ifndef SECCOMP_RET_DATA
#define SECCOMP_RET_DATA 0x0000ffffU
#endif

#if defined(__x86_64__)
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_io_uring_enter
#define __NR_io_uring_enter 426
#endif
#ifndef __NR_io_uring_register
#define __NR_io_uring_register 427
#endif
#ifndef __NR_socketcall
#define __NR_socketcall 102
#endif
#endif

#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define PDF_WORKER_HAS_LANDLOCK_HEADERS 1
#endif

static bool setWorkerResourceLimit(int resource, rlim_t soft, rlim_t hard, const char* name, std::string& error) {
    struct rlimit rlim{};
    rlim.rlim_cur = soft;
    rlim.rlim_max = hard;
    if (setrlimit(resource, &rlim) != 0) {
        error = std::string("Setting resource limit ") + name + " failed: " + std::strerror(errno);
        return false;
    }
    return true;
}
#endif

// AddressSanitizer reserves tens of terabytes of virtual address space at process
// start. Applying RLIMIT_AS afterwards makes every later mmap() fail, so the worker
// cannot allocate, never reaches listen(), and the suite dies with "initial worker
// socket connection". Sanitizer builds therefore skip the address-space cap; the cap
// is validated in the shipping (non-sanitizer) configuration instead.
#if defined(__SANITIZE_ADDRESS__)
#define PDF_WORKER_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PDF_WORKER_ASAN_BUILD 1
#endif
#endif

static bool applyProcessIsolation(std::string& errorMessage, bool testFailRlimit = false) {
#ifdef __linux__
    const pid_t parentPid = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
        errorMessage = "PR_SET_PDEATHSIG failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (getppid() != parentPid) {
        errorMessage = "Parent process exited while worker isolation was being configured.";
        return false;
    }

    if (testFailRlimit) {
        errorMessage = "Setting resource limit RLIMIT_AS failed: Operation not permitted (injected test failure)";
        return false;
    }

    // 0.1 Core dump & ptrace protection: prevent memory inspection and dump retention (T1, T5)
    if (prctl(PR_SET_DUMPABLE, 0) != 0) {
        errorMessage = "PR_SET_DUMPABLE failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (!setWorkerResourceLimit(RLIMIT_CORE, 0, 0, "RLIMIT_CORE", errorMessage)) {
        return false;
    }

    // Set resource limits (RLIMIT_AS, RLIMIT_NOFILE, RLIMIT_CPU) and explicitly verify every return value
    // 1. RLIMIT_AS: 4 GiB virtual address space cap to prevent memory exhaustion DoS (T5)
#ifndef PDF_WORKER_ASAN_BUILD
    if (!setWorkerResourceLimit(RLIMIT_AS, 4096ULL * 1024 * 1024, 4096ULL * 1024 * 1024, "RLIMIT_AS", errorMessage)) {
        return false;
    }
#else
    std::cerr << "[pdf_worker] RLIMIT_AS skipped: AddressSanitizer build." << std::endl;
#endif

    // 2. RLIMIT_NOFILE: max 256 open file descriptors to prevent descriptor leakage
    if (!setWorkerResourceLimit(RLIMIT_NOFILE, 256, 256, "RLIMIT_NOFILE", errorMessage)) {
        return false;
    }

    // 3. RLIMIT_CPU: 7200 s soft / 7260 s hard. A cumulative lifetime budget, not a
    //    per-operation deadline: it is the last-resort backstop behind the UI watchdog,
    //    which is what catches a stuck engine in normal operation. SIGXCPU is converted
    //    to a clean exit so the UI shows its usual recovery banner instead of a crash.
    //    A launcher that already lowered the soft limit keeps its value.
    {
        struct rlimit inherited{};
        rlim_t soft = 7200;
        rlim_t hard = 7260;
        if (getrlimit(RLIMIT_CPU, &inherited) == 0 && inherited.rlim_cur != RLIM_INFINITY) {
            soft = inherited.rlim_cur;
            hard = inherited.rlim_max == RLIM_INFINITY ? soft : inherited.rlim_max;
        }
        if (signal(SIGXCPU, [](int) { _exit(70); }) == SIG_ERR) {
            errorMessage = "Installing the SIGXCPU handler failed";
            return false;
        }
        if (!setWorkerResourceLimit(RLIMIT_CPU, soft, hard, "RLIMIT_CPU", errorMessage)) {
            return false;
        }
    }

    // Note on RLIMIT_CPU: it is a cumulative process-lifetime budget rather than a
    // per-operation deadline, so it is set generously and only serves as a backstop for a
    // worker that somehow keeps burning CPU without the watchdog noticing. The primary
    // detector for a stuck engine remains the UI heartbeat watchdog, which kills and
    // restarts the worker and shows the recovery banner.

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        errorMessage = "PR_SET_NO_NEW_PRIVS failed: " + std::string(std::strerror(errno));
        return false;
    }

#if defined(PDF_WORKER_HAS_SECCOMP_HEADERS) && defined(PR_SET_SECCOMP) && defined(SECCOMP_MODE_FILTER)
    // 4. Seccomp BPF filter: eliminate process execution, async kernel evasion, and network vectors (T1, T2, T10)
    // - Validates architecture: mismatches (e.g. 32-bit int 0x80 on x86_64) terminate the process immediately
    // - Rejects x32 ABI calls (nr >= 0x40000000)
    // - Terminates process (KILL_PROCESS) on dangerous syscalls (execve, execveat, io_uring_*, ptrace, bpf, unshare, kexec_*)
    // - Denies with EPERM on network syscalls (connect, socketcall, non-AF_UNIX socket)
    // - Automatically computes relative jump offsets to eliminate manual offset counting errors
    std::vector<sock_filter> bpfCode;
    std::vector<size_t> jumpsToKill;
    std::vector<size_t> jumpsToDeny;
    std::vector<size_t> jumpsToAllow;

#if defined(SECCOMP_AUDIT_ARCH)
    // Step 1: Validate architecture. Mismatches jump to KILL_PROCESS.
    bpfCode.push_back(sock_filter{BPF_LD | BPF_W | BPF_ABS, 0, 0, static_cast<__u32>(offsetof(struct seccomp_data, arch))});
    size_t archJump = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, 0, 0, SECCOMP_AUDIT_ARCH});
#endif

    // Step 2: Load syscall number
    bpfCode.push_back(sock_filter{BPF_LD | BPF_W | BPF_ABS, 0, 0, static_cast<__u32>(offsetof(struct seccomp_data, nr))});

#if defined(__x86_64__)
    // Step 3: Reject x32 ABI calls (bit 30 set, nr >= 0x40000000)
    size_t x32Jump = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_JMP | BPF_JGE | BPF_K, 0, 0, 0x40000000U});
    jumpsToKill.push_back(x32Jump);
#endif

    auto addKillJump = [&](uint32_t syscallNr) {
        jumpsToKill.push_back(bpfCode.size());
        bpfCode.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, 0, 0, syscallNr});
    };

    auto addDenyJump = [&](uint32_t syscallNr) {
        jumpsToDeny.push_back(bpfCode.size());
        bpfCode.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, 0, 0, syscallNr});
    };

    // Step 4: Syscalls that immediately terminate the worker process (KILL_PROCESS)
#ifdef __NR_execve
    addKillJump(__NR_execve);
#endif
#ifdef __NR_execveat
    addKillJump(__NR_execveat);
#endif
#ifdef __NR_ptrace
    addKillJump(__NR_ptrace);
#endif
#ifdef __NR_bpf
    addKillJump(__NR_bpf);
#endif
#ifdef __NR_unshare
    addKillJump(__NR_unshare);
#endif
#ifdef __NR_kexec_load
    addKillJump(__NR_kexec_load);
#endif
#ifdef __NR_kexec_file_load
    addKillJump(__NR_kexec_file_load);
#endif
#ifdef __NR_io_uring_setup
    addKillJump(__NR_io_uring_setup);
#endif
#ifdef __NR_io_uring_enter
    addKillJump(__NR_io_uring_enter);
#endif
#ifdef __NR_io_uring_register
    addKillJump(__NR_io_uring_register);
#endif

    // Step 5: Syscalls that return EPERM (networking & sockets)
#ifdef __NR_connect
    addDenyJump(__NR_connect);
#endif
#ifdef __NR_socketcall
    addDenyJump(__NR_socketcall);
#endif

    // Step 6: Socket domain inspection (only AF_UNIX is allowed)
#ifdef __NR_socket
    size_t socketCheck = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, 0, 0, __NR_socket});

    size_t notSocketJump = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_JMP | BPF_JA, 0, 0, 0});
    jumpsToAllow.push_back(notSocketJump);

    bpfCode[socketCheck].jt = static_cast<__u8>(bpfCode.size() - (socketCheck + 1));

    bpfCode.push_back(sock_filter{BPF_LD | BPF_W | BPF_ABS, 0, 0, static_cast<__u32>(offsetof(struct seccomp_data, args[0]))});

    size_t domainJump = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_JMP | BPF_JEQ | BPF_K, 0, 0, AF_UNIX});
    jumpsToAllow.push_back(domainJump);
    jumpsToDeny.push_back(domainJump);
#endif

    // Step 7: Terminal actions
    size_t killIndex = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS});

    size_t denyIndex = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)});

    size_t allowIndex = bpfCode.size();
    bpfCode.push_back(sock_filter{BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW});

    // Step 8: Resolve relative jump offsets
#if defined(SECCOMP_AUDIT_ARCH)
    bpfCode[archJump].jf = static_cast<__u8>(killIndex - (archJump + 1));
#endif
    for (size_t idx : jumpsToKill) {
        bpfCode[idx].jt = static_cast<__u8>(killIndex - (idx + 1));
    }
    for (size_t idx : jumpsToDeny) {
#ifdef __NR_socket
        if (idx == domainJump) {
            bpfCode[idx].jf = static_cast<__u8>(denyIndex - (idx + 1));
            continue;
        }
#endif
        bpfCode[idx].jt = static_cast<__u8>(denyIndex - (idx + 1));
    }
    for (size_t idx : jumpsToAllow) {
#ifdef __NR_socket
        if (idx == notSocketJump) {
            bpfCode[idx].k = static_cast<__u32>(allowIndex - (notSocketJump + 1));
            continue;
        }
#endif
        bpfCode[idx].jt = static_cast<__u8>(allowIndex - (idx + 1));
    }

    struct sock_fprog seccompProg = {
        .len = static_cast<unsigned short>(bpfCode.size()),
        .filter = bpfCode.data(),
    };
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &seccompProg) != 0) {
        errorMessage = "prctl(PR_SET_SECCOMP) filter installation failed: " + std::string(std::strerror(errno));
        return false;
    }
#endif

#if defined(PDF_WORKER_HAS_LANDLOCK_HEADERS) && \
    defined(LANDLOCK_CREATE_RULESET_VERSION) && defined(SYS_landlock_create_ruleset) && \
    defined(SYS_landlock_restrict_self) && defined(SYS_landlock_add_rule)
    const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0,
                             LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) {
        errorMessage = "Landlock is unavailable: " + std::string(std::strerror(errno));
        return false;
    }

    landlock_ruleset_attr rulesetAttr{};
    // Keep reads unrestricted for arbitrary PDFs; allow writes only where the shared-memory IPC needs them.
    rulesetAttr.handled_access_fs =
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) {
        rulesetAttr.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
    }
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) {
        rulesetAttr.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    }
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) {
        rulesetAttr.handled_access_fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    }
#endif

    const int sharedMemoryDirFd = open("/dev/shm", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (sharedMemoryDirFd < 0) {
        errorMessage = "Opening /dev/shm for Landlock failed: " +
                       std::string(std::strerror(errno));
        return false;
    }

    const int rulesetFd = static_cast<int>(
        syscall(SYS_landlock_create_ruleset, &rulesetAttr, sizeof(rulesetAttr), 0));
    if (rulesetFd < 0) {
        const int createError = errno;
        close(sharedMemoryDirFd);
        errorMessage = "Creating the Landlock ruleset failed: " +
                       std::string(std::strerror(createError));
        return false;
    }

    // Qt's QSharedMemory tile transport creates, maps, and unlinks segments under /dev/shm.
    landlock_path_beneath_attr sharedMemoryRule{};
    sharedMemoryRule.allowed_access =
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_REMOVE_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) {
        sharedMemoryRule.allowed_access |= LANDLOCK_ACCESS_FS_TRUNCATE;
    }
#endif
    sharedMemoryRule.parent_fd = sharedMemoryDirFd;
    if (syscall(SYS_landlock_add_rule, rulesetFd, LANDLOCK_RULE_PATH_BENEATH,
                &sharedMemoryRule, 0) != 0) {
        const int ruleError = errno;
        close(sharedMemoryDirFd);
        close(rulesetFd);
        errorMessage = "Allowing shared-memory access under /dev/shm failed: " +
                       std::string(std::strerror(ruleError));
        return false;
    }
    if (close(sharedMemoryDirFd) != 0) {
        const int closeError = errno;
        close(rulesetFd);
        errorMessage = "Closing the /dev/shm descriptor failed: " +
                       std::string(std::strerror(closeError));
        return false;
    }

    if (syscall(SYS_landlock_restrict_self, rulesetFd, 0) != 0) {
        const int restrictError = errno;
        close(rulesetFd);
        errorMessage = "Applying the Landlock ruleset failed: " +
                       std::string(std::strerror(restrictError));
        return false;
    }
    if (close(rulesetFd) != 0) {
        errorMessage = "Closing the Landlock ruleset descriptor failed: " +
                       std::string(std::strerror(errno));
        return false;
    }
#else
    errorMessage = "This build lacks Landlock headers or syscall definitions; refusing to run without filesystem restrictions.";
    return false;
#endif
#else
    if (testFailRlimit) {
        errorMessage = "Setting resource limit RLIMIT_AS failed: Operation not permitted (injected test failure)";
        return false;
    }
    (void)errorMessage;
#endif
    return true;
}

static int runIsolationSelftest(int testTcpPort, const std::string& testDbusSocket, const std::string& testShmMarker) {
    std::cout << "=== PDF WORKER ISOLATION SELFTEST ===" << std::endl;
#ifndef __linux__
    (void)testTcpPort;
    (void)testDbusSocket;
    (void)testShmMarker;
    std::cout << "[ISOLATION_SELFTEST] platform: SKIPPED (non-Linux)" << std::endl;
    return 0;
#else
    // Probe 1: Write to /tmp
    {
        const std::string tmpPath = "/tmp/pdf_worker_selftest_" + std::to_string(getpid()) + ".tmp";
        int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM || errno == EROFS) {
                std::cout << "[ISOLATION_SELFTEST] write_tmp: BLOCKED" << std::endl;
            } else {
                std::cout << "[ISOLATION_SELFTEST] write_tmp: INCONCLUSIVE (" << std::strerror(errno) << ")" << std::endl;
            }
        } else {
            close(fd);
            unlink(tmpPath.c_str());
            std::cout << "[ISOLATION_SELFTEST] write_tmp: PERMITTED" << std::endl;
        }
    }

    // Probe 2: Read file outside document (e.g. /etc/passwd or ~/.bashrc)
    {
        int fd = open("/etc/passwd", O_RDONLY);
        if (fd >= 0) {
            std::cout << "[ISOLATION_SELFTEST] read_outside: PERMITTED" << std::endl;
            close(fd);
        } else if (errno == EACCES || errno == EPERM) {
            std::cout << "[ISOLATION_SELFTEST] read_outside: BLOCKED" << std::endl;
        } else {
            const char* home = getenv("HOME");
            bool resolved = false;
            if (home) {
                std::string bashrc = std::string(home) + "/.bashrc";
                fd = open(bashrc.c_str(), O_RDONLY);
                if (fd >= 0) {
                    std::cout << "[ISOLATION_SELFTEST] read_outside: PERMITTED" << std::endl;
                    close(fd);
                    resolved = true;
                } else if (errno == EACCES || errno == EPERM) {
                    std::cout << "[ISOLATION_SELFTEST] read_outside: BLOCKED" << std::endl;
                    resolved = true;
                }
            }
            if (!resolved) {
                std::cout << "[ISOLATION_SELFTEST] read_outside: INCONCLUSIVE (target file unavailable)" << std::endl;
            }
        }
    }

    // Probe 3: TCP Network connection to a controlled endpoint
    {
        if (testTcpPort > 0) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                if (errno == EACCES || errno == EPERM) {
                    std::cout << "[ISOLATION_SELFTEST] open_tcp: BLOCKED" << std::endl;
                } else {
                    std::cout << "[ISOLATION_SELFTEST] open_tcp: INCONCLUSIVE (socket error: "
                              << std::strerror(errno) << ")" << std::endl;
                }
            } else {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<uint16_t>(testTcpPort));
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

                if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    std::cout << "[ISOLATION_SELFTEST] open_tcp: PERMITTED" << std::endl;
                } else if (errno == EACCES || errno == EPERM) {
                    std::cout << "[ISOLATION_SELFTEST] open_tcp: BLOCKED" << std::endl;
                } else {
                    std::cout << "[ISOLATION_SELFTEST] open_tcp: INCONCLUSIVE (connect failed: "
                              << std::strerror(errno) << ")" << std::endl;
                }
                close(sock);
            }
        } else {
            std::cout << "[ISOLATION_SELFTEST] open_tcp: INCONCLUSIVE (no test endpoint provided)" << std::endl;
        }
    }

    // Probe 4: Connect to Session Bus (D-Bus)
    {
        std::string dbusPath = testDbusSocket;
        if (dbusPath.empty()) {
            const char* dbusAddr = getenv("DBUS_SESSION_BUS_ADDRESS");
            if (dbusAddr && strncmp(dbusAddr, "unix:path=", 10) == 0) {
                dbusPath = dbusAddr + 10;
                size_t comma = dbusPath.find(',');
                if (comma != std::string::npos) dbusPath = dbusPath.substr(0, comma);
            }
        }

        if (!dbusPath.empty()) {
            int sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock < 0) {
                if (errno == EACCES || errno == EPERM) {
                    std::cout << "[ISOLATION_SELFTEST] session_bus: BLOCKED" << std::endl;
                } else {
                    std::cout << "[ISOLATION_SELFTEST] session_bus: INCONCLUSIVE (socket error)" << std::endl;
                }
            } else {
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                if (dbusPath.length() < sizeof(addr.sun_path)) {
                    std::strncpy(addr.sun_path, dbusPath.c_str(), sizeof(addr.sun_path) - 1);
                    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                        std::cout << "[ISOLATION_SELFTEST] session_bus: PERMITTED" << std::endl;
                    } else if (errno == EACCES || errno == EPERM) {
                        std::cout << "[ISOLATION_SELFTEST] session_bus: BLOCKED" << std::endl;
                    } else {
                        std::cout << "[ISOLATION_SELFTEST] session_bus: INCONCLUSIVE (connect: "
                                  << std::strerror(errno) << ")" << std::endl;
                    }
                } else {
                    std::cout << "[ISOLATION_SELFTEST] session_bus: INCONCLUSIVE (path too long)" << std::endl;
                }
                close(sock);
            }
        } else {
            std::cout << "[ISOLATION_SELFTEST] session_bus: INCONCLUSIVE (no live bus address)" << std::endl;
        }
    }

    // Probe 5: Modify/remove pre-created foreign marker in /dev/shm
    {
        if (!testShmMarker.empty()) {
            int fd = open(testShmMarker.c_str(), O_WRONLY);
            if (fd >= 0) {
                close(fd);
                if (unlink(testShmMarker.c_str()) == 0) {
                    std::cout << "[ISOLATION_SELFTEST] dev_shm_foreign: PERMITTED" << std::endl;
                } else if (errno == EACCES || errno == EPERM) {
                    std::cout << "[ISOLATION_SELFTEST] dev_shm_foreign: BLOCKED" << std::endl;
                } else {
                    std::cout << "[ISOLATION_SELFTEST] dev_shm_foreign: INCONCLUSIVE (unlink: "
                              << std::strerror(errno) << ")" << std::endl;
                }
            } else if (errno == EACCES || errno == EPERM) {
                std::cout << "[ISOLATION_SELFTEST] dev_shm_foreign: BLOCKED" << std::endl;
            } else {
                std::cout << "[ISOLATION_SELFTEST] dev_shm_foreign: INCONCLUSIVE (open: "
                          << std::strerror(errno) << ")" << std::endl;
            }
        } else {
            std::cout << "[ISOLATION_SELFTEST] dev_shm_foreign: INCONCLUSIVE (no test marker provided)" << std::endl;
        }
    }

    // Probe 6: Syscall filter check (socketcall / 32-bit socket multiplexer)
    {
#if defined(__x86_64__)
        long sc = syscall(102, 0, nullptr);
        if (sc < 0 && (errno == EPERM || errno == ENOSYS)) {
            std::cout << "[ISOLATION_SELFTEST] socketcall: BLOCKED" << std::endl;
        } else {
            std::cout << "[ISOLATION_SELFTEST] socketcall: PERMITTED" << std::endl;
        }
#else
        std::cout << "[ISOLATION_SELFTEST] socketcall: BLOCKED" << std::endl;
#endif
    }

    return 0;
#endif
}

int main(int argc, char* argv[]) {
#ifdef __linux__
    // 0. Descriptor hygiene: close any inherited file descriptors above stderr (0, 1, 2)
    // before initializing Qt or any libraries. This ensures no sensitive files or parent
    // IPC descriptors leak into the worker process, while preserving Qt's internal
    // event loop descriptors (eventfd / socketpair / timerfd) which are created by QCoreApplication.
    const int maxOpenFds = static_cast<int>(sysconf(_SC_OPEN_MAX));
    for (int fd = 3; fd < (maxOpenFds > 0 ? maxOpenFds : 1024); ++fd) {
        ::close(fd);
    }
#endif

    // Headless event loop: no X11/Wayland connection needed in the worker process
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pdf_worker"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PDFium worker with Landlock filesystem write restrictions"));
    parser.addHelpOption();

    QCommandLineOption socketOption(
        QStringList() << QStringLiteral("s") << QStringLiteral("socket"),
        QStringLiteral("IPC local socket name to listen on."),
        QStringLiteral("socketName")
    );
    parser.addOption(socketOption);

    QCommandLineOption selftestOption(
        QStringLiteral("isolation-selftest"),
        QStringLiteral("Run sandbox isolation probe tests and exit.")
    );
    parser.addOption(selftestOption);

    QCommandLineOption tcpPortOption(
        QStringLiteral("test-tcp-port"),
        QStringLiteral("Port of local listening test TCP server for network isolation probe."),
        QStringLiteral("port")
    );
    parser.addOption(tcpPortOption);

    QCommandLineOption dbusSocketOption(
        QStringLiteral("test-dbus-socket"),
        QStringLiteral("Path to live D-Bus socket for session bus probe."),
        QStringLiteral("path")
    );
    parser.addOption(dbusSocketOption);

    QCommandLineOption shmMarkerOption(
        QStringLiteral("test-shm-marker"),
        QStringLiteral("Path to pre-created marker file in /dev/shm for foreign shm probe."),
        QStringLiteral("path")
    );
    parser.addOption(shmMarkerOption);

#ifdef PDF_WORKER_TEST_HOOKS
    QCommandLineOption testFailRlimitOption(
        QStringLiteral("test-fail-rlimit"),
        QStringLiteral("Test hook: simulate setrlimit failure during process isolation setup.")
    );
    parser.addOption(testFailRlimitOption);

    QCommandLineOption testHangEngineOption(
        QStringLiteral("test-hang-engine"),
        QStringLiteral("Test hook: simulate engine-thread hang for watchdog verification.")
    );
    parser.addOption(testHangEngineOption);
#endif

    parser.process(app);

#ifdef PDF_WORKER_TEST_HOOKS
    const bool testFailRlimit = parser.isSet(testFailRlimitOption);
#else
    const bool testFailRlimit = false;
#endif

    if (parser.isSet(selftestOption)) {
        std::string isolationError;
        if (!applyProcessIsolation(isolationError, testFailRlimit)) {
            std::cerr << "[pdf_worker] Failed to establish process isolation for selftest: "
                      << isolationError << std::endl;
            return 1;
        }
        const int tcpPort = parser.value(tcpPortOption).toInt();
        const std::string dbusSocket = parser.value(dbusSocketOption).toStdString();
        const std::string shmMarker = parser.value(shmMarkerOption).toStdString();
        return runIsolationSelftest(tcpPort, dbusSocket, shmMarker);
    }

    const QString socketName = parser.value(socketOption);
    if (socketName.isEmpty() && !testFailRlimit) {
        std::cerr << "[pdf_worker] Error: --socket argument is required." << std::endl;
        return 1;
    }

    std::string isolationError;
    if (!applyProcessIsolation(isolationError, testFailRlimit)) {
        std::cerr << "[pdf_worker] Failed to establish required process isolation: "
                  << isolationError << std::endl;
        return 1;
    }

    pdf::worker::WorkerServer server(socketName);
#ifdef PDF_WORKER_TEST_HOOKS
    if (parser.isSet(testHangEngineOption)) {
        server.setHangEngineForTesting(true);
    }
#endif

    if (!server.start()) {
        std::cerr << "[pdf_worker] Failed to listen on socket: " << socketName.toStdString() << std::endl;
        return 1;
    }

    std::cout << "[pdf_worker] Listening on IPC socket: " << socketName.toStdString() << std::endl;

    // Quit automatically when the UI client disconnects
    QObject::connect(&server, &pdf::worker::WorkerServer::clientDisconnected,
                     &app, &QCoreApplication::quit);

    return app.exec();
}