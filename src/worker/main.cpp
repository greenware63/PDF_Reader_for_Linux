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
    parser.process(app);

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
