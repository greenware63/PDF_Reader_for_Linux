# Threat Model: Linux PDF Reader

**Target Architecture Baseline**: Commit `5a749be` (Linux, Qt 6 Widgets, PDFium `chromium/8100`)  
**Security Focus**: Process isolation, memory confinement, denial of service mitigation, and defense-in-depth against malicious PDF parsing.

---

## 1. Scope and Security Goals

A PDF that exploits a vulnerability in the parsing engine must **not** be able to read the user's private files, modify them, execute arbitrary binaries, or access the network.

### Priority Goals
1. **Containment**: Code execution inside the PDF worker process remains strictly confined: no arbitrary filesystem reads or writes, no process spawning, no network access, and bounded memory consumption.
2. **UI Process Integrity**: The UI process (`pdf-reader`) controls the display, system clipboard, and file dialogs. A hostile or compromised worker must not be able to crash, hang, or corrupt the UI process.
3. **Availability**: A malicious PDF may terminate its own tab's worker, but must not freeze the application or exhaust machine resources.
4. **Privacy**: Document content, encryption passwords, and user file paths must not leak over the network or persist in crash dumps.

### Platform Support Disclosure
- **Linux**: Primary supported platform for secure handling of untrusted PDFs. Employs Linux Landlock (strict read and write confinement), `PR_SET_NO_NEW_PRIVS`, `PR_SET_PDEATHSIG`, `PR_SET_DUMPABLE=0`, `RLIMIT_CORE=0`, `RLIMIT_AS=4GiB`, `RLIMIT_CPU=7200s`, `RLIMIT_NOFILE=256`, seccomp `execve` denial, descriptor passing via `SCM_RIGHTS`, strict descriptor hygiene, and abstract socket IPC.
- **Landlock Compatibility & Fail-Closed Enforcement**: Linux kernels without Landlock support (pre-5.13 or configs without `CONFIG_SECURITY_LANDLOCK=y`) strictly **fail closed** on startup with an explicit diagnostic error. Reduced-isolation fallback modes are intentionally omitted to prevent silent degradation of security boundaries.
- **Windows**: Separate process execution over `QLocalSocket` is supported for functionality and crash isolation, but OS-level sandboxing (Landlock, seccomp, rlimits) is Linux-specific. **Windows builds should currently be considered unsupported for secure processing of untrusted or hostile PDFs.**

---

## 2. Architecture and Trust Boundaries

```
+-------------------------------------------------------------------------+
|                              UI Process                                  |
|                             (pdf-reader)                                |
|  - Holds display, user file dialogs, clipboard, window management       |
|  - Links only to pdf_layout and pdf_ipc (ZERO PDFium linkage)            |
|  - Opens user PDFs read-only (O_CLOEXEC) and passes fd via SCM_RIGHTS    |
|  - Closes local fd immediately after transfer for descriptor hygiene    |
|  - Non-blocking heartbeat watchdog monitoring engine operations          |
+-------------------------------------------------------------------------+
                                    |
          Framed IPC Socket (AF_UNIX) + SCM_RIGHTS + Shared Memory
                     64 MiB Payload Cap, Typed Serialization
                                    |
+-------------------------------------------------------------------------+
|                          pdf_worker Process                             |
|  - Headless worker running RenderWorker engine thread                   |
|  - Links to libpdfium (chromium/8100 pinned)                            |
|  - Sandboxed via Landlock ABI 1-5 (deny reads & writes)                 |
|  - Whitelisted read-only paths: system fonts, fontconfig, locales, tz   |
|  - Scoped read/write path: /dev/shm (shared memory tile transport)      |
|  - Cannot open arbitrary files by path; loads exclusively from passed fd|
|  - <= 512 MiB: heap buffer copy provides disk-immutability (no SIGBUS)  |
|  - > 512 MiB: pread-backed FPDF_FILEACCESS stream with EINTR retry      |
|  - Reads one packet at a time; a descriptor belongs to the packet it    |
|    arrived with. Only OpenDocumentRequest may carry one (exactly one);  |
|    every other descriptor is closed and the message dropped              |
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
| **T1** | Compromised worker reads arbitrary user files (SSH keys, browser profiles) | **High** | Landlock read confinement (`LANDLOCK_ACCESS_FS_READ_FILE \| LANDLOCK_ACCESS_FS_READ_DIR`) blocks all unallowlisted filesystem reads. Worker cannot open arbitrary files by path. Documents are opened exclusively by UI process and passed via `SCM_RIGHTS` (received with `MSG_CMSG_CLOEXEC`). Descriptor lifecycle in the worker: it never reads past the end of the packet being assembled, so a descriptor can only be attached to the packet whose first byte it travelled with (coalesced packets cannot swap descriptors). Descriptors on any message type other than `OpenDocumentRequest`, a second descriptor on an Open, descriptors on malformed or invalid-header packets, and descriptors still pending when the client disconnects are all closed immediately; there is no queue of spare descriptors, so a stray descriptor can never be consumed by a later Open. A packet carrying more than 4 descriptors disconnects the client. Only verified runtime dependencies (fonts, fontconfig, locales, timezone, `/dev/shm`) are whitelisted. Files $\le 512\text{ MiB}$ are copied into immutable heap memory; files $> 512\text{ MiB}$ use pread streaming with `FPDF_FILEACCESS`. | **CLOSED**: Arbitrary file opens in worker return `EACCES`. Verified in `test_worker_ipc.cpp` (Test 8 probe asserts `read_outside: BLOCKED`; Test 15 verifies valid document descriptor passing and rendering; Test 16 verifies opening without descriptor is blocked; Tests 17 and 19 verify descriptor hygiene and association) and `test_render.cpp` (Test 22 verifies `loadFromDescriptor` buffered and pread streaming). |
| **T2** | Compromised worker exfiltrates data over network | **High** | Seccomp BPF filter validating architecture, killing on mismatch or x32 ABI, denying `connect`, `socketcall`, `io_uring_*`, and restricting `socket` domain to `AF_UNIX`. | **Partially Mitigated / Hardened**: Outbound TCP/UDP socket creation and connection are blocked via seccomp BPF (denying `connect`, `socketcall`, non-`AF_UNIX` `socket`, and killing on architecture mismatch or `io_uring_*`). Verified in `test_worker_ipc.cpp` (Test 8 TCP probe and socketcall probe BLOCKED). Dedicated network namespace isolation (`CLONE_NEWNET`) is on the post-MVP roadmap to eliminate remaining local datagram avenues. |
| **T3** | Rogue local process connects to worker socket before UI does | **Medium** | Worker listens on abstract socket `pdf_worker_<pid>_<uuid>`. | **Mitigated**: Socket names use random UUIDs generated per worker. `WorkerServer` enforces single-client semantics (`m_clientConnected`), immediately rejecting and closing any second connection attempt. Phase 4 migrates to inherited `socketpair()`. Verified in `test_worker_ipc.cpp` (Test 1, Test 6). |
| **T4** | Compromised worker tampers with or overwrites files in `/dev/shm` | **Medium** | Landlock allows create/read/write/remove in `/dev/shm` for tile transfer. | **Mitigated**: Worker creates tiles with `0600` permissions (restricted to the same UID). Tile names follow a strict UUID scheme (`pdf-tile-<pid>-<rand>`), verified by `isValidSharedTileKey()`. Reader verifies segment sizes with `fstat` before mapping and immediately unlinks segments. Verified in `test_worker_ipc.cpp` (Test 0 hostile key rejection). |
| **T5** | Malicious PDF causes runaway memory consumption or hangs forever | **Medium** | PDFium rendering complex vector artwork or decompression bombs. | **CLOSED**: Enforces `RLIMIT_AS = 4 GiB`, `RLIMIT_CPU = 7200s` with clean `_exit(70)` on `SIGXCPU`. Non-blocking UI watchdog queries worker health every 2s. If an operation stalls without progress for $> 60\text{ s}$ (or 3 missed pings), worker is killed and recovery banner displayed. Verified in `test_worker_ipc.cpp` (Test 9A, 9B, 9C, 9D, 10). |
| **T6** | Hostile worker output exploits UI process deserializers | **Medium** | Serialized messages delivered from worker to UI process. | **Partially Mitigated / Hardened**: Bounded framing (`MAX_IPC_PAYLOAD_SIZE = 64 MiB`), strict magic (`0x50444657`), version matching, integer overflow checks (`validateCollection`), and clamping on dimensions (16,384 px max). Verified in `test_worker_ipc.cpp` (Test 0, Test 3) and `IpcFuzzTestSuite` (1000+ mutated deserialization fuzz runs). Continuous coverage-guided fuzzing (libFuzzer/AFL++) in CI remains on the roadmap. |
| **T7** | Unpatched PDFium vulnerabilities remain exploitable | **Medium** | Prebuilt binary pinned to Chromium release. | **Mitigated**: Pinned build is explicitly tracked at `chromium/8100` and downloaded with SHA256 integrity verification. |
| **T8** | Worker fails to start on kernels lacking Landlock | **Low** (Availability) | Worker enforces fail-closed isolation. | **Mitigated by Design**: Fail-closed policy in `applyProcessIsolation()`. Worker aborts with clear diagnostic error if Landlock or required sandboxing is unsupported, preventing unconfined execution. Verified in `test_worker_ipc.cpp` (Test 10, Test 18). |
| **T9** | Shell argument injection during page printing | **Low** | PDF metadata or printer naming passed to external printing subsystems. | **Mitigated**: Printing uses Qt's structured `QPrinterInfo` and `QPrinter` APIs without shell invocations. |
| **T10** | Worker reaches D-Bus session bus or desktop socket | **Medium** | Desktop environment session bus accessible via abstract or `/tmp` sockets. | **Partially Mitigated / Hardened**: Stream connection to D-Bus session bus and desktop sockets is blocked via seccomp (`connect` returns `EPERM`). Verified in `test_worker_ipc.cpp` (Test 8 D-Bus probe BLOCKED). Unconnected datagrams are restricted by lack of reachable service endpoints. |
| **T11** | Password and document path leakage in settings or logs | **Low** | Document encryption passwords and recent file lists. | **Mitigated**: Worker does not log password strings or request bodies. "Clear History" function in UI removes persisted document paths. Passwords travel in-memory only over local IPC. |
| **T12** | Incomplete redaction or residual content leakage in redacted output | **High** | Burn-in redaction pipeline (`src/redact/Redaction.cpp`) rasterizing and overwriting pixels. | **Mitigated**: Structural sanitization reconstructing output PDF from empty document with pure image XObjects; zero text operators, font programs, OCR streams, annotations, or metadata survive (G1–G10). Output is bounded by limitations L1–L14 in `docs/REDACTION.md` (lossy, raster-only, resolution-bound, edge-growth coverage). UI confirmation dialog explicitly informs users that the *entire document (all pages)* is rasterized into image-only pages. UI selection applies a 1.5 pt safety margin to bound descenders, diacritics, and italic overhang. Target overwrite blocked by `safety::isSameFile()`. Verified in `RedactionTestSuite` (including its mutation checks), `test_redaction_rotation` and, for the selection margin, `ViewportWidgetTestSuite` (`test_widget.cpp`). |

---

## 4. Test Verification Matrix

Every security control is mapped directly to regression unit tests executed under CTest:

- **Test 0**: Shared-tile name sanitization (rejects traversal, illegal prefix, shrunken segment SIGBUS defense).
- **Test 1**: Worker process lifecycle, versioned handshake, and Ping/Pong negotiation.
- **Test 2**: IPC document loading and password handling with passed descriptor.
- **Test 3**: Shared-memory rasterization with memory bounds validation.
- **Test 6**: Clean teardown on socket disconnect without zombie processes.
- **Test 7**: Crash resilience (worker termination caught cleanly without crashing the UI).
- **Test 8**: Deterministic isolation escape probe (verifies write confinement to `/tmp`, read confinement asserting `read_outside: BLOCKED`, D-Bus socket reachability, foreign `/dev/shm` marker).
- **Test 9A**: Watchdog progress snapshot verification during simulated engine hang.
- **Test 9B**: Watchdog re-arming across document close/reopen and live stall timeout detection.
- **Test 9C**: Progressing multi-tile operations complete smoothly without false watchdog triggers.
- **Test 9D**: Workload validation under high-DPI rendering asserting `VmPeak` $< 4\text{ GiB}$ `RLIMIT_AS`.
- **Test 10**: Explicit abort and diagnostic error handling when resource limits fail to apply (`--test-fail-rlimit`).
- **Test 11**: End-to-end sandboxed worker rasterization and redaction payload verification.
- **Test 12**: Malformed redaction request error reporting.
- **Test 13**: Redaction immunity to on-disk source file mutation/deletion after opening.
- **Test 14**: Long redaction progress heartbeats keep watchdog and client silence limit alive.
- **Test 15**: Valid document reads and rendering work via descriptor passing over IPC.
- **Test 16**: Denied reads of unrelated files (Landlock read confinement blocks arbitrary file opens without descriptor).
- **Test 17**: A Ping carrying an unsolicited descriptor is dropped and the connection stays usable.
- **Test 19**: Descriptor lifecycle, checked from outside the worker (the test hands over the read end of a pipe and detects closure through POLLERR on the write end): 19A descriptors on Ping/Close/Cancel/Outline are closed and the request dropped; 19B a stray descriptor is not consumed by a later Open; 19C queued, coalesced packets keep each descriptor with its own packet (worker stopped with SIGSTOP while packets accumulate); 19D `[Ping][Open]` in one `sendmsg` binds the descriptor to the Ping, so the Open fails closed; 19E two in-flight Opens are matched in order; 19F an Open with two descriptors is rejected and both are closed; 19G malformed Open payload: descriptor closed and protocol stays in sync; 19H invalid header: descriptor closed and connection dropped; 19I disconnect mid-packet: pending descriptor closed.
- **Test 18**: Worker startup strictly fails closed when Landlock is unavailable (`--test-fail-landlock`).
- **`test_render` Test 22**: `loadFromDescriptor` handles invalid fds, buffered loading ($\le 512\text{ MiB}$), and pread streaming ($> 512\text{ MiB}$).
- **Redaction Verification**: Structural sanitization, canary elimination, and independent parser inspection in `RedactionTestSuite`; UI rotation and file overwrite protection in `test_redaction_rotation` and `test_redaction_file_safety`; the 1.5 pt selection margin in `test_widget.cpp` (`ViewportWidgetTestSuite`).
