#!/usr/bin/env bash
# Local CI runner reproducing all four Ubuntu 24.04 CI legs from .github/workflows/ci.yml
# In order:
#   1. Release (ENABLE_TEST_HOOKS=ON)
#   2. AddressSanitizer / UndefinedBehaviorSanitizer (ASan/UBSan)
#   3. ThreadSanitizer (TSan)
#   4. Shipping config (ENABLE_TEST_HOOKS=OFF, binary hardening checks)
#
# Exits non-zero immediately on any configuration, build, assertion, or test failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Determine base build directory: prefer environment variable, container path, or local fallback
if [ -n "${BUILD_ROOT:-}" ]; then
    BASE_BUILD="${BUILD_ROOT}"
elif [ -d "/home/ubuntu/build" ] && [ -w "/home/ubuntu/build" ]; then
    BASE_BUILD="/home/ubuntu/build"
else
    BASE_BUILD="${REPO_ROOT}/.build-ci-local"
fi

echo "======================================================================"
echo " Starting Local CI Suite (Repository: ${REPO_ROOT})"
echo " Build directory root: ${BASE_BUILD}"
echo "======================================================================"

# Global environment settings matching CI
export QT_QPA_PLATFORM="offscreen"
export REDACT_REQUIRE_WITNESSES="1"
export CI="1"

# ----------------------------------------------------------------------
# Leg 1: Release (hooks ON)
# ----------------------------------------------------------------------
echo ""
echo ">>> [1/4] Leg 1: Release (hooks ON)"
BUILD_DIR_REL="${BASE_BUILD}/release"
cmake -B "${BUILD_DIR_REL}" -S "${REPO_ROOT}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TEST_HOOKS=ON

cmake --build "${BUILD_DIR_REL}"

echo "Checking test-hook presence in hooks-ON worker..."
for flag in test-hang-engine test-fail-rlimit test-fail-landlock test-probe-read; do
    strings -el "${BUILD_DIR_REL}/pdf_worker" | grep -q -e "$flag" || {
        echo "ERROR: Expected hook flag '$flag' not found in ${BUILD_DIR_REL}/pdf_worker" >&2
        exit 1
    }
done

echo "Running CTest for Release leg..."
ctest --test-dir "${BUILD_DIR_REL}" --output-on-failure --verbose -R "^ExternalEngineCompatTest$"
ctest --test-dir "${BUILD_DIR_REL}" --output-on-failure

# ----------------------------------------------------------------------
# Leg 2: AddressSanitizer / UndefinedBehaviorSanitizer
# ----------------------------------------------------------------------
echo ""
echo ">>> [2/4] Leg 2: AddressSanitizer / UBSan"
BUILD_DIR_ASAN="${BASE_BUILD}/asan"
cmake -B "${BUILD_DIR_ASAN}" -S "${REPO_ROOT}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_TEST_HOOKS=ON \
    -DENABLE_SANITIZERS=asan

cmake --build "${BUILD_DIR_ASAN}"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

echo "Running CTest for ASan/UBSan leg..."
ctest --test-dir "${BUILD_DIR_ASAN}" --output-on-failure --verbose -R "^ExternalEngineCompatTest$"
ctest --test-dir "${BUILD_DIR_ASAN}" --output-on-failure

# ----------------------------------------------------------------------
# Leg 3: ThreadSanitizer
# ----------------------------------------------------------------------
echo ""
echo ">>> [3/4] Leg 3: ThreadSanitizer"
BUILD_DIR_TSAN="${BASE_BUILD}/tsan"
cmake -B "${BUILD_DIR_TSAN}" -S "${REPO_ROOT}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENABLE_TEST_HOOKS=ON \
    -DENABLE_SANITIZERS=tsan

cmake --build "${BUILD_DIR_TSAN}"

export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:suppressions=${REPO_ROOT}/ci/tsan_suppressions.txt"

# Modern kernels (Linux 6.6+, e.g. WSL2/Docker kernel 6.18) use high-entropy ASLR
# which can conflict with ThreadSanitizer's static virtual memory map.
# Relax ASLR via `setarch $(uname -m) -R` if available to prevent mapping aborts.
TSAN_PREFIX=""
if command -v setarch >/dev/null 2>&1; then
    TSAN_PREFIX="setarch $(uname -m) -R"
fi

echo "Running CTest for TSan leg (single-process suites)..."
${TSAN_PREFIX} ctest --test-dir "${BUILD_DIR_TSAN}" --output-on-failure -R "EngineRenderTestSuite|DocumentLayoutTestSuite|TileCacheTestSuite"

# ----------------------------------------------------------------------
# Leg 4: Shipping config (hooks OFF)
# ----------------------------------------------------------------------
echo ""
echo ">>> [4/4] Leg 4: Shipping config (hooks OFF)"
BUILD_DIR_SHIP="${BASE_BUILD}/shipping"
cmake -B "${BUILD_DIR_SHIP}" -S "${REPO_ROOT}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release

echo "Asserting fault-injection hooks are disabled..."
cmake -LA -N "${BUILD_DIR_SHIP}" | grep -E "^ENABLE_TEST_HOOKS:BOOL=OFF$"

cmake --build "${BUILD_DIR_SHIP}"

echo "Asserting hook flags are compiled out of shipping worker..."
for flag in test-hang-engine test-fail-rlimit test-fail-landlock test-probe-read; do
    if strings -el "${BUILD_DIR_SHIP}/pdf_worker" | grep -q -e "$flag"; then
        echo "ERROR: pdf_worker still contains the --$flag hook in shipping build" >&2
        exit 1
    fi
done

echo "Asserting binary hardening (PIE, full RELRO, BIND_NOW)..."
for bin in pdf-reader pdf_worker; do
    f="${BUILD_DIR_SHIP}/$bin"
    readelf -hW "$f" | grep -q 'Type:.*DYN' || { echo "ERROR: $bin is not PIE" >&2; exit 1; }
    readelf -lW "$f" | grep -q 'GNU_RELRO' || { echo "ERROR: $bin has no RELRO segment" >&2; exit 1; }
    readelf -dW "$f" | grep -q -E 'BIND_NOW|FLAGS_1.*NOW' || { echo "ERROR: $bin is not linked -z now" >&2; exit 1; }
done

echo "Running CTest for Shipping leg..."
ctest --test-dir "${BUILD_DIR_SHIP}" --output-on-failure

echo ""
echo "======================================================================"
echo " ALL FOUR CI LEGS PASSED SUCCESSFULLY!"
echo "======================================================================"
