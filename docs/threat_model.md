# Threat Model: Linux PDF Reader

**Target Architecture Baseline**: Commit `5a749be` (Linux, Qt 6 Widgets, PDFium `chromium/8100`)  
**Security Focus**: Process isolation, memory confinement, denial of service mitigation, and defense-in-depth against malicious PDF parsing.

---

## 1. Scope and Security Goals

A PDF that exploits a vulnerability in the parsing engine must **not** be able to read the user's private files, modify them, execute arbitrary binaries, or access the network.

### Priority Goals
1. **Containment**: Code execution inside the PDF worker process remains strictly confined: no arbitrary filesystem writes, no process spawning, no network access, and bounded memory consumption.
2. **UI Process Integrity**: The UI process (`pdf-reader`) controls the display, system clipboard, and file dialogs. A hostile or compromised worker must not be able to crash, hang, or corrupt the UI process.
3. **Availability**: A malicious PDF may terminate its own tab's worker, but must not freeze the application or exhaust machine resources.
4. **Privacy**: Document content, encryption passwords, and user file paths must not leak over the network or persist in crash dumps.

### Platform Support Disclosure
- **Linux**: Primary supported platform for secure handling of untrusted PDFs. Employs Linux Landlock, `PR_SET_NO_NEW_PRIVS`, `PR_SET_PDEATHSIG`, `PR_SET_DUMPABLE=0`, `RLIMIT_CORE=0`, `RLIMIT_AS=4GiB`, `RLIMIT_CPU=7200s`, `RLIMIT_NOFILE=256`, seccomp `execve` denial, descriptor hygiene, and abstract socket IPC.
- **Windows**: Separate process execution over `QLocalSocket` is supported for functionality and crash isolation, but OS-level sandboxing (Landlock, seccomp, rlimits) is Linux-specific. **Windows builds should currently be considered unsupported for secure processing of untrusted or hostile PDFs.**

---

## 2. Architecture and Trust Boundaries

```
+-------------------------------------------------------------------------+
|                              UI Process                                  |
|                             (pdf-reader)                                |
|  - Holds display, user file dialogs, clipboard, window management       |
|  - Links only to pdf_layout and pdf_ipc (ZERO PDFium linkage)            |
|  - Non-blocking heartbeat watchdog monitoring engine operations          |
+-------------------------------------------------------------------------+
                                    |
          Framed IPC Socket (QLocalSocket) + Shared Memory Transport
                     64 MiB Payload Cap, Typed Serialization
                                    |
+-------------------------------------------------------------------------+
|                          pdf_worker Process                             |
|  - Headless worker running RenderWorker engine thread                   |
|  - Links to libpdfium (chromium/8100 pinned)                            |
|  - Sandboxed via Landlock ABI 1-3 (deny writes, scoped /dev/shm)        |
|  - PR_SET_NO_NEW_PRIVS = 1                                             |
|  - PR_SET_DUMPABLE = 0, RLIMIT_CORE = 0 (prevents core dumps/ptrace)   |
|  - RLIMIT_AS = 4 GiB, RLIMIT_CPU = 7200s (SIGXCPU -> _exit(70))         |
|  - RLIMIT_NOFILE = 256 (with pre-sandbox descriptor hygiene)            |
|  - Seccomp BPF filter denying execve and execveat                        |
|  - Heartbeat responder answering with lock-free progress snapshots      |
+-------------------------------------------------------------------------+
```

---

## 3. Threat Analysis & Hardening Status (T1 – T11)

| Threat ID | Threat & Impact | Severity | Code Evidence & Mechanism | Mitigation Status & Test Verification |
|---|---|---|---|---|
| **T1** | Compromised worker reads arbitrary user files (SSH keys, browser profiles) | **High** | Reads remain unconfined so worker can open arbitrary user-chosen PDF paths directly. | **Partially Mitigated / Phase 4 Roadmap**: Core-dump exposure disabled via `PR_SET_DUMPABLE=0` and `RLIMIT_CORE=0`. Descriptor hygiene closes all fds $> 2$ on startup. Verified in `test_worker_ipc.cpp` (Test 8 escape probe). Phase 4 migrates document opening to the UI process, passing an open file descriptor via `SCM_RIGHTS`. |
| **T2** | Compromised worker exfiltrates data over network | **High** | Seccomp BPF filter denying `connect` and non-`AF_UNIX` `socket` creation (`AF_INET`, `AF_INET6`, etc.). | **CLOSED**: Seccomp BPF filter installed at startup denying `connect` (TCP/UDP) and non-`AF_UNIX` `socket()` syscalls, completely preventing network communication. Process spawning via `execve`/`execveat` remains denied. Verified in `test_worker_ipc.cpp` (Test 8 TCP probe BLOCKED). |
| **T3** | Rogue local process connects to worker socket before UI does | **Medium** | Worker listens on abstract socket `pdf_worker_<pid>_<uuid>`. | **Mitigated**: Socket names use random UUIDs generated per worker. `WorkerServer` enforces single-client semantics (`m_clientConnected`), immediately rejecting and closing any second connection attempt. Phase 4 migrates to inherited `socketpair()`. Verified in `test_worker_ipc.cpp` (Test 1, Test 6). |
| **T4** | Compromised worker tampers with or overwrites files in `/dev/shm` | **Medium** | Landlock allows create/write/remove in `/dev/shm` for tile transfer. | **Mitigated**: Worker creates tiles with `0600` permissions (restricted to the same UID). Tile names follow a strict UUID scheme (`pdf-tile-<pid>-<rand>`), verified by `isValidSharedTileKey()`. Reader verifies segment sizes with `fstat` before mapping and immediately unlinks segments. Verified in `test_worker_ipc.cpp` (Test 0 hostile key rejection). |
| **T5** | Malicious PDF causes runaway memory consumption or hangs forever | **Medium** | PDFium rendering complex vector artwork or decompression bombs. | **CLOSED**: Enforces `RLIMIT_AS = 4 GiB`, `RLIMIT_CPU = 7200s` with clean `_exit(70)` on `SIGXCPU`. Non-blocking UI watchdog queries worker health every 2s. If an operation stalls without progress for $> 60\text{ s}$ (or 3 missed pings), worker is killed and recovery banner displayed. Verified in `test_worker_ipc.cpp` (Test 9A, 9B, 9C, 9D, 10). |
| **T6** | Hostile worker output exploits UI process deserializers | **Medium** | Serialized messages delivered from worker to UI process. | **CLOSED**: Bounded framing (`MAX_IPC_PAYLOAD_SIZE = 64 MiB`), strict magic (`0x50444657`), version matching, integer overflow checks (`validateCollection`), and clamping on dimensions (16,384 px max). Unknown message types drop connection. Verified in `test_worker_ipc.cpp` (Test 0, Test 3) and `IpcFuzzTestSuite` (1000+ mutated deserialization fuzz runs). |
| **T7** | Unpatched PDFium vulnerabilities remain exploitable | **Medium** | Prebuilt binary pinned to Chromium release. | **Mitigated**: Pinned build is explicitly tracked at `chromium/8100` and downloaded with SHA256 integrity verification. |
| **T8** | Worker fails to start on kernels lacking Landlock | **Low** (Availability) | Worker enforces fail-closed isolation. | **Mitigated by Design**: Fail-closed policy in `applyProcessIsolation()`. Worker aborts with clear diagnostic error if Landlock or required sandboxing is unsupported, preventing unconfined execution. Verified in `test_worker_ipc.cpp` (Test 10). |
| **T9** | Shell argument injection during page printing | **Low** | PDF metadata or printer naming passed to external printing subsystems. | **Mitigated**: Printing uses Qt's structured `QPrinterInfo` and `QPrinter` APIs without shell invocations. |
| **T10** | Worker reaches D-Bus session bus or desktop socket | **Medium** | Desktop environment session bus accessible via abstract or `/tmp` sockets. | **CLOSED**: Outbound connections denied via seccomp BPF filter on `connect()`, blocking connection to D-Bus session bus sockets and desktop service endpoints. Verified in `test_worker_ipc.cpp` (Test 8 D-Bus probe BLOCKED). |
| **T11** | Password and document path leakage in settings or logs | **Low** | Document encryption passwords and recent file lists. | **Mitigated**: Worker does not log password strings or request bodies. "Clear History" function in UI removes persisted document paths. Passwords travel in-memory only over local IPC. |

---

## 4. Test Verification Matrix

Every security control is mapped directly to regression unit tests executed under CTest:

- **Test 0**: Shared-tile name sanitization (rejects traversal, illegal prefix, shrunken segment SIGBUS defense).
- **Test 1**: Worker process lifecycle, versioned handshake, and Ping/Pong negotiation.
- **Test 2**: IPC document loading and password handling.
- **Test 3**: Shared-memory rasterization with memory bounds validation.
- **Test 6**: Clean teardown on socket disconnect without zombie processes.
- **Test 7**: Crash resilience (worker termination caught cleanly without crashing the UI).
- **Test 8**: Deterministic isolation escape probe (verifies write confinement to `/tmp`, baseline read access, D-Bus socket reachability, foreign `/dev/shm` marker).
- **Test 9A**: Watchdog progress snapshot verification during simulated engine hang.
- **Test 9B**: Watchdog re-arming across document close/reopen and live stall timeout detection.
- **Test 9C**: Progressing multi-tile operations complete smoothly without false watchdog triggers.
- **Test 9D**: Workload validation under high-DPI rendering asserting `VmPeak` $< 4\text{ GiB}$ `RLIMIT_AS`.
- **Test 10**: Explicit abort and diagnostic error handling when sandbox limits fail to apply.
