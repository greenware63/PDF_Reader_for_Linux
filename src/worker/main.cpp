#include "WorkerServer.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <iostream>

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#endif
#endif

static void applyProcessIsolation() {
#ifdef __linux__
    // 1. Terminate immediately if parent (UI process) dies or crashes
    prctl(PR_SET_PDEATHSIG, SIGKILL);

    // 2. Prevent acquiring new privileges (mandatory for unprivileged seccomp / Landlock)
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

#if defined(LANDLOCK_CREATE_RULESET_VERSION) && defined(SYS_landlock_create_ruleset)
    // Check if Landlock ABI is supported by current running kernel
    int abi = static_cast<int>(syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
    if (abi > 0) {
        struct landlock_ruleset_attr ruleset_attr = {};
        ruleset_attr.handled_access_fs =
            LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR;

        int ruleset_fd = static_cast<int>(syscall(SYS_landlock_create_ruleset, &ruleset_attr, sizeof(ruleset_attr), 0));
        if (ruleset_fd >= 0) {
            syscall(SYS_landlock_restrict_self, ruleset_fd, 0);
            close(ruleset_fd);
        }
    }
#endif
#endif
}

int main(int argc, char* argv[]) {
    // Headless event loop: no X11/Wayland connection needed in the worker process
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pdf_worker"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    applyProcessIsolation();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Sandboxed PDFium rendering engine worker process"));
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
