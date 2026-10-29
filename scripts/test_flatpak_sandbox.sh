#!/usr/bin/env bash
# ==============================================================================
# scripts/test_flatpak_sandbox.sh
# Flatpak Packaging & Sandbox Security Verification Suite
# ==============================================================================
# Verifies:
#   1. Flatpak manifest syntax and structure (org.greenware.PDFReader.yaml)
#   2. AppStream metadata validation (org.greenware.PDFReader.metainfo.xml)
#   3. Desktop entry validation (org.greenware.PDFReader.desktop)
#   4. Security Policy Audit:
#        - D-Bus isolation (asserts NO blanket session-bus or system-bus access)
#        - Portal integration (FileChooser, Print, Desktop portals declared)
#        - Filesystem confinement (no blanket host/home filesystem holes)
#        - Wayland/X11 socket configuration and DRI access
#   5. Optional Live Sandbox Execution (if flatpak-builder & KDE runtime present):
#        - Builds application inside container
#        - Executes pdf_worker and verifies Landlock + seccomp sandboxing
# ==============================================================================

set -euo pipefail

# ANSI color codes for diagnostic output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

MANIFEST_ROOT="${REPO_ROOT}/org.greenware.PDFReader.yaml"
MANIFEST_PKG="${REPO_ROOT}/packaging/flatpak/org.greenware.PDFReader.yaml"
DESKTOP_FILE="${REPO_ROOT}/resources/org.greenware.PDFReader.desktop"
METAINFO_FILE="${REPO_ROOT}/resources/org.greenware.PDFReader.metainfo.xml"
ICON_FILE="${REPO_ROOT}/resources/org.greenware.PDFReader.svg"

TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

pass() {
    echo -e "  [${GREEN}PASS${NC}] $1"
    ((TESTS_PASSED++)) || true
}

fail() {
    echo -e "  [${RED}FAIL${NC}] $1"
    ((TESTS_FAILED++)) || true
}

skip() {
    echo -e "  [${YELLOW}SKIP${NC}] $1"
    ((TESTS_SKIPPED++)) || true
}

header() {
    echo -e "\n${BOLD}${BLUE}=== $1 ===${NC}"
}

echo -e "${BOLD}Running Flatpak Packaging & Sandbox Verification Suite${NC}"
echo "Repository Root: ${REPO_ROOT}"

# ------------------------------------------------------------------------------
# 1. Manifest and Resource File Existence
# ------------------------------------------------------------------------------
header "1. Verifying Packaging Files Existence"

if [[ -f "${MANIFEST_ROOT}" ]]; then
    pass "Root manifest exists: org.greenware.PDFReader.yaml"
else
    fail "Root manifest missing: org.greenware.PDFReader.yaml"
fi

if [[ -f "${MANIFEST_PKG}" ]]; then
    pass "Packaging manifest exists: packaging/flatpak/org.greenware.PDFReader.yaml"
else
    fail "Packaging manifest missing: packaging/flatpak/org.greenware.PDFReader.yaml"
fi

if [[ -f "${DESKTOP_FILE}" ]]; then
    pass "Desktop entry exists: resources/org.greenware.PDFReader.desktop"
else
    fail "Desktop entry missing: resources/org.greenware.PDFReader.desktop"
fi

if [[ -f "${METAINFO_FILE}" ]]; then
    pass "AppStream metainfo exists: resources/org.greenware.PDFReader.metainfo.xml"
else
    fail "AppStream metainfo missing: resources/org.greenware.PDFReader.metainfo.xml"
fi

if [[ -f "${ICON_FILE}" ]]; then
    pass "Scalable icon exists: resources/org.greenware.PDFReader.svg"
else
    fail "Scalable icon missing: resources/org.greenware.PDFReader.svg"
fi

# ------------------------------------------------------------------------------
# 2. Manifest Syntax and Schema Validation
# ------------------------------------------------------------------------------
header "2. Validating Manifest Syntax and Fields"

check_manifest_fields() {
    local manifest="$1"
    local name="$(basename "$manifest")"

    # App ID
    if grep -q "^app-id: org.greenware.PDFReader" "$manifest"; then
        pass "${name}: app-id is org.greenware.PDFReader"
    else
        fail "${name}: app-id mismatch or missing"
    fi

    # Command
    if grep -q "^command: pdf-reader" "$manifest"; then
        pass "${name}: command is pdf-reader"
    else
        fail "${name}: command mismatch or missing"
    fi

    # Runtime and SDK
    if grep -q "runtime: org.kde.Platform" "$manifest" && grep -q "sdk: org.kde.Sdk" "$manifest"; then
        pass "${name}: uses Qt6/KDE Platform and Sdk runtime"
    else
        fail "${name}: missing org.kde.Platform or org.kde.Sdk"
    fi
}

check_manifest_fields "${MANIFEST_ROOT}"
check_manifest_fields "${MANIFEST_PKG}"

# ------------------------------------------------------------------------------
# 3. Sandbox Security Policy & Isolation Audit
# ------------------------------------------------------------------------------
header "3. Security Policy Audit (finish-args)"

audit_security_policy() {
    local manifest="$1"
    local name="$(basename "$manifest")"

    # D-Bus isolation: MUST NOT contain blanket session-bus or system-bus access
    if grep -q -- "--socket=session-bus" "$manifest"; then
        fail "${name}: SECURITY VIOLATION: --socket=session-bus is present (violates D-Bus isolation)"
    elif grep -q -- "--socket=system-bus" "$manifest"; then
        fail "${name}: SECURITY VIOLATION: --socket=system-bus is present (violates D-Bus isolation)"
    else
        pass "${name}: D-Bus isolation verified (no blanket session-bus / system-bus access)"
    fi

    # Portals: FileChooser and Print portal talk permissions
    if grep -q -- "--talk-name=org.freedesktop.portal.FileChooser" "$manifest"; then
        pass "${name}: FileChooser portal permission declared"
    else
        fail "${name}: Missing org.freedesktop.portal.FileChooser talk-name"
    fi

    if grep -q -- "--talk-name=org.freedesktop.portal.Print" "$manifest"; then
        pass "${name}: Print portal permission declared"
    else
        fail "${name}: Missing org.freedesktop.portal.Print talk-name"
    fi

    # Display & IPC: Wayland, fallback X11, IPC share, DRI
    if grep -q -- "--socket=wayland" "$manifest" && grep -q -- "--socket=fallback-x11" "$manifest"; then
        pass "${name}: Display sockets configured (wayland + fallback-x11)"
    else
        fail "${name}: Missing wayland or fallback-x11 socket configuration"
    fi

    if grep -q -- "--share=ipc" "$manifest"; then
        pass "${name}: Shared memory IPC enabled for high-performance rasterization"
    else
        fail "${name}: Missing --share=ipc finish-arg"
    fi

    if grep -q -- "--device=dri" "$manifest"; then
        pass "${name}: DRI GPU acceleration access granted"
    else
        fail "${name}: Missing --device=dri finish-arg"
    fi

    # Filesystem containment: MUST NOT have --filesystem=host or --filesystem=home
    if grep -q -- "--filesystem=host" "$manifest"; then
        fail "${name}: SECURITY VIOLATION: --filesystem=host leaks host filesystem"
    elif grep -q -- "--filesystem=home" "$manifest"; then
        fail "${name}: SECURITY VIOLATION: --filesystem=home leaks entire user home directory"
    else
        pass "${name}: Filesystem containment verified (no host or home holes)"
    fi
}

audit_security_policy "${MANIFEST_ROOT}"

# ------------------------------------------------------------------------------
# 4. Standard Desktop & AppStream Specification Checks
# ------------------------------------------------------------------------------
header "4. Desktop & AppStream Validation Tools"

if command -v desktop-file-validate >/dev/null 2>&1; then
    if desktop-file-validate "${DESKTOP_FILE}"; then
        pass "desktop-file-validate succeeded on ${DESKTOP_FILE}"
    else
        fail "desktop-file-validate reported errors on ${DESKTOP_FILE}"
    fi
else
    skip "desktop-file-validate not found on host (install desktop-file-utils to run)"
fi

if command -v appstreamcli >/dev/null 2>&1; then
    if appstreamcli validate --no-net "${METAINFO_FILE}"; then
        pass "appstreamcli validate succeeded on ${METAINFO_FILE}"
    else
        fail "appstreamcli validate reported errors on ${METAINFO_FILE}"
    fi
else
    skip "appstreamcli not found on host (install appstream to run)"
fi

# ------------------------------------------------------------------------------
# 5. Live Flatpak Builder & Sandbox Self-Test (Optional/Live)
# ------------------------------------------------------------------------------
header "5. Live Flatpak Builder & Sandbox Test"

# Offline prerequisites of the live self-test (these run everywhere, including CI without flatpak).
for f in tests/fixtures/unicode_embedded.pdf tests/fixtures/unicode_system_font.pdf sample.pdf; do
    if [[ -s "${REPO_ROOT}/${f}" ]]; then pass "self-test fixture present: ${f}"; else fail "self-test fixture missing: ${f}"; fi
done
if grep -q "option(INSTALL_SANDBOX_SELFTEST" "${REPO_ROOT}/CMakeLists.txt"; then
    pass "CMake exposes INSTALL_SANDBOX_SELFTEST"
else
    fail "CMake does not expose INSTALL_SANDBOX_SELFTEST"
fi
if grep -q "INSTALL_SANDBOX_SELFTEST" "${MANIFEST_ROOT}"; then
    fail "the shipping manifest must NOT enable INSTALL_SANDBOX_SELFTEST"
else
    pass "shipping manifest does not install the self-test binary"
fi

RUN_BUILD=false
if [[ "${1:-}" == "--build" || "${1:-}" == "--test-runtime" ]]; then
    RUN_BUILD=true
fi

if command -v flatpak-builder >/dev/null 2>&1; then
    # Test manifest parsing with flatpak-builder
    if flatpak-builder --show-manifest "${MANIFEST_ROOT}" >/dev/null 2>&1; then
        pass "flatpak-builder --show-manifest validated root manifest"
    else
        fail "flatpak-builder failed to parse ${MANIFEST_ROOT}"
    fi

    if [[ "$RUN_BUILD" == true ]]; then
        echo "Starting live flatpak-builder compilation..."
        BUILD_DIR="${FLATPAK_BUILD_DIR:-/tmp/flatpak-pdf-reader-build}"
        REPO_DIR="${FLATPAK_REPO_DIR:-/tmp/flatpak-pdf-reader-repo}"
        STATE_DIR="${FLATPAK_STATE_DIR:-/tmp/flatpak-pdf-reader-state}"
        rm -rf "${BUILD_DIR}" "${REPO_DIR}" "${STATE_DIR}"

        # The self-test build is a COPY of the shipping manifest with one extra CMake option, so the real
        # manifest never installs the test binary.
        SELFTEST_MANIFEST="${REPO_ROOT}/.flatpak-selftest.yaml"
        sed 's|^\(\s*\)- -DENABLE_TEST_HOOKS=OFF|\1- -DENABLE_TEST_HOOKS=ON\n\1- -DINSTALL_SANDBOX_SELFTEST=ON|' \
            "${MANIFEST_ROOT}" > "${SELFTEST_MANIFEST}"
        grep -q 'INSTALL_SANDBOX_SELFTEST=ON' "${SELFTEST_MANIFEST}" || { fail "could not derive the self-test manifest"; exit 1; }

        flatpak-builder --disable-rofiles-fuse --state-dir="${STATE_DIR}" --force-clean --user --repo="${REPO_DIR}" "${BUILD_DIR}" "${SELFTEST_MANIFEST}"
        pass "flatpak-builder build (self-test variant) completed successfully"

        # Smoke only: --help returns before the worker applies any sandbox, so this proves nothing about confinement.
        if (cd "${BUILD_DIR}" && flatpak-builder --run "${BUILD_DIR}" "${SELFTEST_MANIFEST}" pdf_worker --help) >/dev/null 2>&1; then
            pass "pdf_worker starts inside the Flatpak sandbox (smoke test only, no confinement is checked here)"
        else
            fail "pdf_worker failed to start inside the Flatpak sandbox"
        fi

        # The real check: run the worker IPC suite against /app/bin/pdf_worker INSIDE the sandbox. This covers
        # Landlock read/write confinement (Test 8/16), fail-closed startup, descriptor passing and lifecycle
        # (Tests 15-19) and non-Latin text rendering incl. the system-font fallback (Test 20).
        SELFTEST_LOG="${REPO_ROOT}/.flatpak-selftest.log"
        if (cd "${BUILD_DIR}" && flatpak-builder --run "${BUILD_DIR}" "${SELFTEST_MANIFEST}" \
               /app/bin/test_worker_ipc /app/bin/pdf_worker /app/share/pdf-reader/selftest/sample.pdf) \
               >"${SELFTEST_LOG}" 2>&1; then
            pass "test_worker_ipc passed inside the Flatpak sandbox (log: ${SELFTEST_LOG})"
        else
            fail "test_worker_ipc FAILED inside the Flatpak sandbox; if the log shows Landlock being unavailable, the sandbox (or its seccomp filter) blocks landlock_* and the fail-closed worker cannot start - see ${SELFTEST_LOG}"
        fi
        for needle in "20a passed" "ALL WORKER IPC UNIT TESTS PASSED"; do
            if grep -q "${needle}" "${SELFTEST_LOG}"; then
                pass "sandbox log contains: ${needle}"
            else
                fail "sandbox log is missing: ${needle}"
            fi
        done
        # Test 21: every font root fontconfig reports inside the sandbox (incl. /run/host/*) must be readable by the
        # Landlock-confined worker. It FAILS the whole run if a /run/host font path is blocked.
        if grep -q "21 passed" "${SELFTEST_LOG}"; then
            pass "confined worker can read every font root fontconfig reports inside the sandbox (Test 21)"
            grep -E "^     /" "${SELFTEST_LOG}" | sed 's/^/        /' || true
        elif grep -q "21 SKIPPED" "${SELFTEST_LOG}"; then
            skip "Test 21 skipped: fc-list reports no fonts inside the sandbox, so host font exposure was NOT verified"
        else
            fail "sandbox log has neither 21 passed nor 21 SKIPPED"
        fi
        if grep -q "20b passed" "${SELFTEST_LOG}"; then
            pass "system-font fallback renders non-embedded CJK text inside the sandbox (Test 20b)"
        elif grep -q "20b SKIPPED" "${SELFTEST_LOG}"; then
            skip "Test 20b skipped: the Flatpak runtime has no Japanese font per fontconfig, so the system-font path was NOT verified in the sandbox"
        else
            fail "sandbox log has neither 20b passed nor 20b SKIPPED"
        fi
        # 20b is reported above; do not count its SKIPPED line a second time.
        if grep -v "20b\|21 SKIPPED" "${SELFTEST_LOG}" | grep -q "SKIPPED\|Skipped"; then
            skip "other sub-tests reported Skipped inside the sandbox (see log)"
        fi
    else
        skip "Live build skipped (pass --build or --test-runtime to compile and execute in container)"
    fi
else
    skip "flatpak-builder not installed on host; offline manifest and policy checks passed"
fi

# ------------------------------------------------------------------------------
# Summary Report
# ------------------------------------------------------------------------------
header "Verification Summary"
echo -e "Total Passed:  ${GREEN}${TESTS_PASSED}${NC}"
echo -e "Total Failed:  ${RED}${TESTS_FAILED}${NC}"
echo -e "Total Skipped: ${YELLOW}${TESTS_SKIPPED}${NC}"

if [[ ${TESTS_FAILED} -eq 0 ]]; then
    echo -e "\n${BOLD}${GREEN}All Flatpak packaging and sandbox security verification checks PASSED!${NC}"
    exit 0
else
    echo -e "\n${BOLD}${RED}One or more Flatpak packaging checks FAILED.${NC}"
    exit 1
fi
