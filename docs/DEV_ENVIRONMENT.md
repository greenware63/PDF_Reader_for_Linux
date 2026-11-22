# Linux Development Environment (Dev Container & Local CI)

This document describes how to build, run, and debug the Linux PDF Reader Workstation locally in an environment equivalent to the `ubuntu-24.04` GitHub Actions CI runners, with deliberate differences noted below.

---

## 1. Architecture & Container Specifications

The development container is defined in [`.devcontainer/Dockerfile`](../.devcontainer/Dockerfile) and [`.devcontainer/devcontainer.json`](../.devcontainer/devcontainer.json).

### Base & Toolchain
- **Base image**: `ubuntu:24.04` (Noble Numbat).
- **Core CI dependencies**: `build-essential`, `cmake`, `ninja-build`, `qt6-base-dev`, `libgl1-mesa-dev`, `libxkbcommon-dev`, `qpdf`, `poppler-utils`, `mupdf-tools`.
- **Developer & testing additions**:
  - `zlib1g-dev`: required for `test_redaction` (`RedactionTestSuite`) independent inflate verification (pre-installed in GitHub Actions runner VM, but omitted in bare `ubuntu:24.04` images).
  - `git`: repository tracking, hash verification, and CMake FetchContent.
  - `python3`: parity matrix and test verification scripts.
  - `gdb`: native interactive debugging inside CLion and container.
  - `clang` / `llvm`: llvm-symbolizer for AddressSanitizer and ThreadSanitizer stack traces.
  - `sudo`: passwordless administrative access for the non-root `ubuntu` user (`UID 1000`).

### Deliberate Differences from GitHub Actions CI Runners
The container environment is equivalent to CI, with these deliberate differences:
1. **`seccomp=unconfined`**: Docker's default seccomp profile intercepts/blocks syscalls like `landlock_create_ruleset` (syscall 444), `landlock_add_rule`, and `landlock_restrict_self`, causing fail-closed worker initialization errors. Disabling container-level seccomp allows the worker to apply its own fine-grained Seccomp BPF and Landlock filters. GitHub Actions runners execute directly on host VMs without container seccomp filtering.
2. **`--cap-add=SYS_PTRACE`**: Required for AddressSanitizer (ASan), ThreadSanitizer (TSan), and GDB breakpoint inspection in containerized environments.
3. **`--shm-size=1g`**: Expands shared memory (`/dev/shm`) from Docker's default 64 MiB to 1 GiB to accommodate multi-process concurrent tile rasters during test runs. GitHub Actions runner VMs allocate 50% of system RAM to `/dev/shm` by default.
4. **`setarch -R` for TSan**: On modern Linux kernels (Linux 6.6+, such as kernel 6.18 on WSL2 / Docker Desktop), high-entropy 32-bit ASLR pushes mmap allocations into regions conflicting with ThreadSanitizer's static virtual memory map (`FATAL: ThreadSanitizer: unexpected memory mapping`). Running TSan CTest under `setarch $(uname -m) -R` relaxes ASLR for the test process tree.
5. **Host Kernel & Landlock ABI**: Inside the local Docker/WSL2 container, the host kernel is `6.18.58.1-microsoft-standard-WSL2`, which reports **Landlock ABI 7**. Stock Ubuntu 24.04 GitHub Actions runners typically run Linux kernel 6.8 (which reports Landlock ABI 3 or 4). Both satisfy the application's requirement for Landlock ABI 1–5.

### Storage & Build Directory Separation
When running on Windows with Docker Desktop, the source code repository is mounted from the host. Build artifacts must **not** be placed directly on the Windows 9p/virtiofs mount due to high I/O latency and file locking.
- Build artifacts reside on a dedicated Docker volume mounted at `/home/ubuntu/build` (native Linux ext4 filesystem).

---

## 2. Opening in CLion

CLion natively supports Dev Containers via Docker:

1. **Prerequisites**: Ensure Docker Desktop is running with Linux containers enabled.
2. **Open Project**: Open the repository directory in CLion.
3. **Dev Container Detection**: CLion detects `.devcontainer/devcontainer.json` and displays a notification banner: *"Dev Container configuration found"*.
4. **Select Toolchain**:
   - Go to **Settings / Preferences** (`Ctrl+Alt+S`) -> **Build, Execution, Deployment** -> **Toolchains**.
   - Select or add **Docker**. Set the Docker image to the project's dev container (or click *"Create from devcontainer.json"*).
   - CMake and C++ Compiler will automatically detect `/usr/bin/cmake`, `/usr/bin/ninja`, and `/usr/bin/g++`.
5. **CMake Profiles**:
   - In **Settings** -> **Build, Execution, Deployment** -> **CMake**, configure build profiles targeting the Docker toolchain:
     - **Debug**: `-DCMAKE_BUILD_TYPE=Debug -DENABLE_TEST_HOOKS=ON`
     - **Release**: `-DCMAKE_BUILD_TYPE=Release -DENABLE_TEST_HOOKS=ON`
     - **ASan**: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TEST_HOOKS=ON -DENABLE_SANITIZERS=asan`
     - **TSan**: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TEST_HOOKS=ON -DENABLE_SANITIZERS=tsan`
   - Set the Build Directory to `/home/ubuntu/build/<profile>` so builds execute on the native volume.

---

## 3. Running Local CI (`scripts/ci-local.sh`)

[`scripts/ci-local.sh`](../scripts/ci-local.sh) reproduces the entire four-leg matrix executed by GitHub Actions:

```bash
# Inside the container or native Ubuntu 24.04:
./scripts/ci-local.sh
```

### The 4 Matrix Legs
1. **Leg 1: Release (hooks ON)**:
   - Config: `-DCMAKE_BUILD_TYPE=Release -DENABLE_TEST_HOOKS=ON`
   - Validates worker fault-injection hook strings (`test-hang-engine`, `test-fail-rlimit`, `test-fail-landlock`, `test-probe-read`).
   - Runs `ExternalEngineCompatTest` and all 13 CTest test suites.
2. **Leg 2: AddressSanitizer / UndefinedBehaviorSanitizer (ASan/UBSan)**:
   - Config: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TEST_HOOKS=ON -DENABLE_SANITIZERS=asan`
   - Enforces `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.
   - Runs full test suite under memory sanitization.
3. **Leg 3: ThreadSanitizer (TSan)**:
   - Config: `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TEST_HOOKS=ON -DENABLE_SANITIZERS=tsan`
   - Enforces `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=ci/tsan_suppressions.txt`.
   - Relaxes ASLR via `setarch $(uname -m) -R`.
   - Runs single-process suites (`EngineRenderTestSuite`, `DocumentLayoutTestSuite`, `TileCacheTestSuite`).
4. **Leg 4: Shipping config (hooks OFF)**:
   - Config: `-DCMAKE_BUILD_TYPE=Release -DENABLE_TEST_HOOKS=OFF`
   - Asserts hooks are compiled out of `pdf_worker`.
   - Asserts binary hardening (PIE, Full RELRO, BIND_NOW).
   - Runs test suite (hook-dependent tests skip cleanly).

---

## 4. Running Individual Tests

To run a specific test suite or test target:

```bash
# 1. Build the target
cmake --build /home/ubuntu/build/release --target test_render

# 2. Run the test binary with headless Qt display
QT_QPA_PLATFORM=offscreen CI=1 /home/ubuntu/build/release/test_render

# Or run via CTest:
ctest --test-dir /home/ubuntu/build/release --output-on-failure -R "^EngineRenderTestSuite$"
```

To run under GDB:
```bash
QT_QPA_PLATFORM=offscreen gdb --args /home/ubuntu/build/release/test_render
```
