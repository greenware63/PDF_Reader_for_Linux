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

#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define PDF_WORKER_HAS_LANDLOCK_HEADERS 1
#endif
#endif

static bool applyProcessIsolation(std::string& errorMessage) {
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

    // Set resource limits (RLIMIT_AS, RLIMIT_CPU, RLIMIT_NOFILE) to prevent resource exhaustion (T5)
    // 1. RLIMIT_AS: 4 GiB virtual address space cap
    struct rlimit rlim{};
    rlim.rlim_cur = 4096ULL * 1024 * 1024;
    rlim.rlim_max = 4096ULL * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rlim);

    // 2. RLIMIT_CPU: 60s soft / 120s hard CPU limit
    rlim.rlim_cur = 60;
    rlim.rlim_max = 120;
    setrlimit(RLIMIT_CPU, &rlim);

    // 3. RLIMIT_NOFILE: max 256 open file descriptors
    rlim.rlim_cur = 256;
    rlim.rlim_max = 256;
    setrlimit(RLIMIT_NOFILE, &rlim);

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        errorMessage = "PR_SET_NO_NEW_PRIVS failed: " + std::string(std::strerror(errno));
        return false;
    }

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
    (void)errorMessage;
#endif
    return true;
}

static int runIsolationSelftest() {
    std::cout << "=== PDF WORKER ISOLATION SELFTEST ===" << std::endl;
#ifndef __linux__
    std::cout << "[ISOLATION_SELFTEST] platform: SKIPPED (non-Linux)" << std::endl;
    return 0;
#else
    // Probe 1: Write to /tmp
    bool writeBlocked = false;
    {
        const std::string tmpPath = "/tmp/pdf_worker_selftest_" + std::to_string(getpid()) + ".tmp";
        int fd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            writeBlocked = (errno == EACCES || errno == EPERM || errno == EROFS);
        } else {
            close(fd);
            unlink(tmpPath.c_str());
        }
    }
    std::cout << "[ISOLATION_SELFTEST] write_tmp: " << (writeBlocked ? "BLOCKED" : "PERMITTED") << std::endl;

    // Probe 2: Read file outside document (e.g. /etc/passwd or ~/.bashrc)
    bool readPermitted = false;
    {
        int fd = open("/etc/passwd", O_RDONLY);
        if (fd >= 0) {
            readPermitted = true;
            close(fd);
        } else {
            const char* home = getenv("HOME");
            if (home) {
                std::string bashrc = std::string(home) + "/.bashrc";
                fd = open(bashrc.c_str(), O_RDONLY);
                if (fd >= 0) {
                    readPermitted = true;
                    close(fd);
                }
            }
        }
    }
    std::cout << "[ISOLATION_SELFTEST] read_outside: " << (readPermitted ? "PERMITTED" : "BLOCKED") << std::endl;

    // Probe 3: Open TCP network socket
    bool networkPermitted = false;
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            networkPermitted = true;
            close(sock);
        }
    }
    std::cout << "[ISOLATION_SELFTEST] open_tcp: " << (networkPermitted ? "PERMITTED" : "BLOCKED") << std::endl;

    // Probe 4: Connect to Session Bus (D-Bus)
    bool sessionBusPermitted = false;
    {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock >= 0) {
            const char* dbusAddr = getenv("DBUS_SESSION_BUS_ADDRESS");
            std::string path;
            if (dbusAddr && strncmp(dbusAddr, "unix:path=", 10) == 0) {
                path = dbusAddr + 10;
                size_t comma = path.find(',');
                if (comma != std::string::npos) path = path.substr(0, comma);
            } else {
                path = "/run/user/" + std::to_string(getuid()) + "/bus";
            }
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            if (!path.empty() && path.length() < sizeof(addr.sun_path)) {
                std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
                if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    sessionBusPermitted = true;
                } else if (errno != EACCES && errno != EPERM) {
                    // Socket syscall itself permitted (failed with e.g. ENOENT or ECONNREFUSED)
                    sessionBusPermitted = true;
                }
            } else {
                sessionBusPermitted = true;
            }
            close(sock);
        }
    }
    std::cout << "[ISOLATION_SELFTEST] session_bus: " << (sessionBusPermitted ? "PERMITTED" : "BLOCKED") << std::endl;

    // Probe 5: Delete / create foreign file in /dev/shm
    bool devShmPermitted = false;
    {
        const std::string foreignPath = "/dev/shm/pdf_worker_probe_foreign_" + std::to_string(getpid()) + ".tmp";
        int fd = open(foreignPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            devShmPermitted = true;
            close(fd);
            unlink(foreignPath.c_str());
        }
    }
    std::cout << "[ISOLATION_SELFTEST] dev_shm_foreign: " << (devShmPermitted ? "PERMITTED" : "BLOCKED") << std::endl;

    return 0;
#endif
}

int main(int argc, char* argv[]) {
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

    parser.process(app);

    if (parser.isSet(selftestOption)) {
        std::string isolationError;
        if (!applyProcessIsolation(isolationError)) {
            std::cerr << "[pdf_worker] Failed to establish process isolation for selftest: "
                      << isolationError << std::endl;
            return 1;
        }
        return runIsolationSelftest();
    }

    const QString socketName = parser.value(socketOption);
    if (socketName.isEmpty()) {
        std::cerr << "[pdf_worker] Error: --socket argument is required." << std::endl;
        return 1;
    }

    std::string isolationError;
    if (!applyProcessIsolation(isolationError)) {
        std::cerr << "[pdf_worker] Failed to establish required process isolation: "
                  << isolationError << std::endl;
        return 1;
    }

    pdf::worker::WorkerServer server(socketName);
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