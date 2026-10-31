# Threat Model: Sandboxed Multi-Process PDF Architecture

## 1. Executive Summary

`PDF_Reader_for_Linux` is designed from the ground up to render untrusted, potentially malicious PDF documents safely. Historically, PDF readers and rendering engines (including PDFium) are frequent targets of remote code execution (RCE) exploits due to complex parsers, font renderers, embedded image decoders, and legacy specification cruft.

This project mitigates this attack surface via a **strict privilege-separated multi-process architecture**:
1. **Host UI Process (`pdf-reader`)**: Unconfined native Qt6 application executing with standard user privileges. It handles user interaction, menus, windowing, and display of rendered bitmap tiles. It **never links or executes PDFium code**.
2. **Sandboxed Worker Process (`pdf_worker`)**: Heavily restricted isolated child process. It is the **only component that links and executes PDFium**. It performs parsing, decoding, rasterization, and text extraction. It operates under mandatory Linux kernel sandboxing: Landlock filesystem read confinement, seccomp-BPF system call filtering, `RLIMIT_CORE=0`, `RLIMIT_AS=4GiB`, `RLIMIT_CPU=7200s`, `RLIMIT_NOFILE=256`, seccomp `execve` denial, descriptor passing via `SCM_RIGHTS`, strict descriptor hygiene, and abstract socket IPC.

---

## 2. Process Architecture & Trust Boundaries

```
+-----------------------------------------------------------------------------+
|                           UI PROCESS (Untrusted Input)                      |
|                                                                             |
|  - Full user privileges (Desktop session)                                   |
|  - Handles GUI events, input, windowing (Qt6 Widgets)                       |
|  - Links only to pdf_layout and pdf_ipc (ZERO PDFium linkage)               |
|  - Receives rasterized tile memory / extracts                               |
|  - Spawns & monitors sandboxed worker via watchdog timer (Ping/Pong)       |
+------------------------------------+----------------------------------------+
                                     |
          Framed IPC Socket (AF_UNIX) + SCM_RIGHTS + Shared Memory
                                     |
+------------------------------------+----------------------------------------+
|                      SANDBOXED WORKER PROCESS (pdf_worker)                  |
|                                                                             |
|  - Links PDFium (Chromium pinned release)                                  |
|  - LANDLOCK: Filesystem reads strictly blocked outside approved assets      |
|              (fonts, fontconfig, locales, timezone, /dev/shm)               |
|  - SECCOMP-BPF:                                                             |
|      * Denies network: connect, socketcall, non-AF_UNIX socket             |
|      * Denies process execution: execve, execveat, fork, vfork, clone       |
|      * Blocks dangerous kernel interfaces: io_uring_*, bpf, ptrace, reboot  |
|  - LIMITS: RLIMIT_AS = 4 GiB, RLIMIT_CPU = 7200s, RLIMIT_CORE = 0,         |
|            RLIMIT_NOFILE = 256                                              |
|  - FILE ACCESS: No arbitrary filesystem opens. Documents received via       |
|                 SCM_RIGHTS read-only file descriptors (loadFromDescriptor). |
|  - TILE OUTPUT: Shared memory (/dev/shm) under strict UUID keys             |
+-----------------------------------------------------------------------------+
```

---

## 3. Threat Analysis & Hardening Status (T1 – T14)

| Threat ID | Threat Description | Severity | Mitigation Strategy | Status |
|---|---|---|---|---|
| **T1** | Compromised worker reads arbitrary user files (SSH keys, browser profiles) | **High** | Landlock read confinement (`LANDLOCK_ACCESS_FS_READ_FILE \| LANDLOCK_ACCESS_FS_READ_DIR`) blocks all unallowlisted filesystem reads. Worker cannot open arbitrary files by path. Documents are opened exclusively by UI process and passed via `SCM_RIGHTS` (received with `MSG_CMSG_CLOEXEC`). Descriptor lifecycle in the worker: it never reads past the end of the packet being assembled, so a descriptor can only be attached to the packet whose first byte it travelled with (coalesced packets cannot swap descriptors). Descriptors on any message type other than `OpenDocumentRequest` and `SaveDocumentRequest`, a second descriptor on an Open/Save, write-mode descriptors on Open, read-only descriptors on Save, descriptors on malformed or invalid-header packets, and descriptors still pending when the client disconnects are all closed immediately; there is no queue of spare descriptors, so a stray descriptor can never be consumed by a later request. A packet carrying more than 4 descriptors disconnects the client. Only verified runtime dependencies (fonts, fontconfig, locales, timezone, `/dev/shm`) are whitelisted. Files $\le 512\text{ MiB}$ are copied into immutable heap memory; files $> 512\text{ MiB}$ use pread streaming with `FPDF_FILEACCESS`. | **CLOSED**: Arbitrary file opens in worker return `EACCES`. Verified in `test_worker_ipc.cpp` (Test 8 probe asserts `read_outside: BLOCKED`; Test 15 verifies valid document descriptor passing and rendering; Test 16 verifies opening without descriptor is blocked; Tests 17 and 19 verify descriptor hygiene and association) and `test_render.cpp` (Test 22 verifies `loadFromDescriptor` buffered and pread streaming). |
| **T2** | Compromised worker exfiltrates data over network | **High** | Seccomp BPF filter validating architecture, killing on mismatch or x32 ABI, denying `connect`, `socketcall`, `io_uring_*`, and restricting `socket` domain to `AF_UNIX`. | **Partially Mitigated / Hardened**: Outbound TCP/UDP socket creation and connection are blocked via seccomp BPF (denying `connect`, `socketcall`, non-`AF_UNIX` `socket`, and killing on architecture mismatch or `io_uring_*`). Verified in `test_worker_ipc.cpp` (Test 8 TCP probe and socketcall probe BLOCKED). Dedicated network namespace isolation (`CLONE_NEWNET`) is on the post-MVP roadmap to eliminate remaining local datagram avenues. |
| **T3** | Rogue local process connects to worker socket before UI does | **Medium** | Worker listens on abstract socket `pdf_worker_<pid>_<uuid>`. | **Mitigated**: Socket names use random UUIDs generated per worker. `WorkerServer` enforces single-client semantics (`m_clientConnected`), immediately rejecting and closing any second connection attempt. Phase 4 migrates to inherited `socketpair()`. Verified in `test_worker_ipc.cpp` (Test 1, Test 6). |
| **T4** | Compromised worker tampers with or overwrites files in `/dev/shm` | **Medium** | Landlock allows create/read/write/remove in `/dev/shm` for tile transfer. | **Mitigated**: Worker creates tiles with `0600` permissions (restricted to the same UID). Tile names follow a strict UUID scheme (`pdf-tile-<pid>-<rand>`), verified by `isValidSharedTileKey()`. Reader verifies segment sizes with `fstat` before mapping and immediately unlinks segments. Verified in `test_worker_ipc.cpp` (Test 0 hostile key rejection). |
| **T5** | Malicious PDF causes runaway memory consumption or hangs forever | **Medium** | PDFium rendering complex vector artwork or decompression bombs. | **CLOSED**: Enforces `RLIMIT_AS = 4 GiB`, `RLIMIT_CPU = 7200s` with clean `_exit(70)` on `SIGXCPU`. Non-blocking UI watchdog queries worker health every 2s. If an operation stalls without progress for $> 60\text{ s}$ (or 3 missed pings), worker is killed and recovery banner displayed. Verified in `test_worker_ipc.cpp` (Test 9A, 9B, 9C, 9D, 10). |
| **T6** | Hostile worker output exploits UI process deserializers | **Medium** | Serialized messages delivered from worker to UI process. | **Partially Mitigated / Hardened**: Bounded framing (`MAX_IPC_PAYLOAD_SIZE = 64 MiB`), strict magic (`0x50444657`), version matching, integer overflow checks (`validateCollection`), and clamping on dimensions (16,384 px max). Verified in `test_worker_ipc.cpp` (Test 0, Test 3) and `IpcFuzzTestSuite` (1000+ mutated deserialization fuzz runs). Continuous coverage-guided fuzzing (libFuzzer/AFL++) in CI remains on the roadmap. |
| **T7** | Unpatched PDFium vulnerabilities remain exploitable | **Medium** | Prebuilt binary pinned to Chromium release. | **Mitigated**: Pinned build is explicitly tracked at `chromium/8100` and downloaded with SHA256 integrity verification. |
| **T8** | Worker fails to start on kernels lacking Landlock | **Low** (Availability) | Worker enforces fail-closed isolation. | **Mitigated by Design**: Fail-closed policy in `applyProcessIsolation()`. Worker aborts with clear diagnostic error if Landlock or required sandboxing is unsupported, preventing unconfined execution. Verified in `test_worker_ipc.cpp` (Test 10, Test 18). |
| **T9** | Shell argument injection during page printing | **Low** | PDF metadata or printer naming passed to external printing subsystems. | **Mitigated**: Printing uses Qt's structured `QPrinterInfo` and `QPrinter` APIs without shell invocations. |
| **T10** | Worker reaches D-Bus session bus or desktop socket | **Medium** | Desktop environment session bus accessible via abstract or `/tmp` sockets. | **Partially Mitigated / Hardened**: Stream connection to D-Bus session bus and desktop sockets is blocked via seccomp (`connect` returns `EPERM`). Verified in `test_worker_ipc.cpp` (Test 8 D-Bus probe BLOCKED). Unconnected datagrams are restricted by lack of reachable service endpoints. |\
| **T11** | Password and document path leakage in settings or logs | **Low** | Document encryption passwords and recent file lists. | **Mitigated**: Worker does not log password strings or request bodies. "Clear History" function in UI removes persisted document paths. Passwords travel in-memory only over local IPC. |
| **T12** | Incomplete redaction or residual content leakage in redacted output | **High** | Burn-in redaction pipeline (`src/redact/Redaction.cpp`) rasterizing and overwriting pixels. | **Mitigated**: Structural sanitization reconstructing output PDF from empty document with pure image XObjects; zero text operators, font programs, OCR streams, annotations, or metadata survive (G1–G10). Output is bounded by limitations L1–L14 in `docs/REDACTION.md` (lossy, raster-only, resolution-bound, edge-growth coverage). UI confirmation dialog explicitly informs users that the *entire document (all pages)* is rasterized into image-only pages. UI selection applies a 1.5 pt safety margin to bound descenders, diacritics, and italic overhang. Target overwrite blocked by `safety::isSameFile()`. Verified in `RedactionTestSuite` (including its mutation checks), `test_redaction_rotation` and, for the selection margin, `ViewportWidgetTestSuite` (`test_widget.cpp`). |
| **T13** | Hostile PDF annotations cause out-of-bounds reads, memory exhaustion, or IPC frame bomb session loss | **Medium** | Hostile `/Contents` or `/QuadPoints` arrays crafted to overflow worker buffer, crash deserializers, or breach the 64 MiB IPC frame limit. | **Mitigated**: Engine bounds allocation to exact PDFium returned size capped at 1 MiB hostile limit; caps string decoding to 32,768 UTF-16 code units (64 KiB); caps quads per annotation to 4,096; caps annotations per page to 10,000; enforces a strict 16 MiB per-page IPC serialized budget (`kMaxAnnotationListBytes`), halting extraction and setting `truncated = true` when budget is reached; deserializer independently enforces the 16 MiB budget; ray casting point-in-polygon verifies exact quad hit-testing. Verified in `test_render.cpp` (Tests 25–28) and `test_worker_ipc.cpp` (Tests 22, 23, 24 with hostile fixtures `hostile_annot_big_contents.pdf` and `hostile_annot_many_quads.pdf`). |
| **T14** | Malicious save tampering, late worker writes, or data loss during document save / undo | **High** | Worker descriptor passing during document save, in-place file overwrite, undo state synchronization, and crash recovery. | **Mitigated**: Protocol v7/v8 enforces strict descriptor typing: only `OpenDocumentRequest` (read-only) and `SaveDocumentRequest` (write-only via `fcntl(F_GETFL) & O_ACCMODE`) can receive passed descriptors. Protocol v8 transmits user-space glyph boxes for exact annotation geometry under rotation/cropping. Full rewrites on digitally signed documents are blocked (`signatureCount() > 0`), enforcing incremental save mode. The worker streams data with heartbeats. The UI independently validates written bytes (size, `%PDF-` header, `%%EOF` marker, incremental byte-prefix preservation, and re-parsing in a fresh worker verifying page & annotation counts) before commit. Commit uses `QSaveFile` writing the exact validated in-memory bytes (never re-reading worker files, ignoring late worker writes from held fds; atomic rename; preserving symlinks and target file permissions; zero `.bak` pollution). Protocol v7/v8 `RemoveAnnotationRequest` removes annotations by `/NM` identifier, ensuring undo/redo reaches the document and matches saved bytes. `EditLog::opsSinceSave()` ensures worker crash recovery only replays edits made since the last save. Verified in `test_worker_ipc.cpp` (Tests 25–29), `test_render.cpp` (Tests 29–33), and `test_widget.cpp` (Tests 46–48). |

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
- **Test 21**: Every font root that fontconfig reports (one probe file per root, plus `/run/host/font-dirs.xml` when present) must be readable by the Landlock-confined worker; roots under `/run/host/` that are blocked fail the test, other blocked roots are printed as notes. Uses the hooks-only `--isolation-selftest --test-probe-read`, which opens and reads one byte after sandboxing. Prints `21b SKIPPED` when fontconfig reports no font root under `/run/host/` (not running under Flatpak, or the sandbox maps no host fonts): the test then proves nothing about host fonts. `scripts/test_flatpak_sandbox.sh --test-runtime` installs the self-test build locally and runs the suite with `flatpak run`, so the sandbox is the one users get (host fonts mapped under `/run/host`), and prints `ls /run/host` / fontconfig diagnostics first.
- **`test_render` Test 22**: `loadFromDescriptor` handles invalid fds, buffered loading ($\le 512\text{ MiB}$), and pread streaming ($> 512\text{ MiB}$).
- **`test_worker_ipc` Test 22**: IPC annotation listing, hit-testing, and bounds checking.
- **`test_worker_ipc` Test 23**: Malformed annotation request frames are cleanly rejected without hanging the connection.
- **`test_worker_ipc` Test 24**: Annotation list response budget enforcement: rejects oversized lists at deserializer, verifies `truncated` flag round-trip, and confirms sandboxed worker session survival on hostile PDFs (`hostile_annot_big_contents.pdf` and `hostile_annot_many_quads.pdf`).
- **`test_render` Tests 25–28**: Annotation extraction, widget appearance single-pass invariant, multi-line quad hit-testing, rotated quad ray-casting point-in-polygon, and oversized 100 KiB string truncation.
- **`test_worker_ipc` Tests 25–28**: IPC document save & highlight annotation over IPC with progress heartbeats (Test 25); worker crash recovery via `EditLog::replay()` (Test 26); malformed save/annotation request rejection (Test 27); `RemoveAnnotation` by `/NM` undo protocol (Test 28).
- **`test_render` Tests 29–32**: Highlight annotation creation with `/Rect` prior to `/AP` and page coordinates (Test 29); incremental byte-prefix preservation vs full rewrite (Test 30); redaction interplay obliterating underlying annotations (Test 31); `EditLog` command stack undo/redo operations (Test 32).
- **`test_widget` Tests 46–47**: Highlight selection, `EditLog` dirty tracking, and document save (Test 46); saved-file integrity: undo/redo reaching saved output, in-place atomic commit preserving file mode/symlinks without `.bak` files, late worker write rejection, crash recovery re-applying only unsaved edits, non-reentrant save/edit guards, and user-space coordinate persistence under rotation/cropping (Test 47a–e).
- **Redaction Verification**: Structural sanitization, canary elimination, and independent parser inspection in `RedactionTestSuite`; UI rotation and file overwrite protection in `test_redaction_rotation` and `test_redaction_file_safety`; the 1.5 pt selection margin in `test_widget.cpp` (`ViewportWidgetTestSuite`).
