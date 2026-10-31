cmake_minimum_required(VERSION 3.24)
project(pdf-reader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizer option (test-only; never ship with sanitizers enabled)
# Usage: -DENABLE_SANITIZERS=asan  or  -DENABLE_SANITIZERS=tsan
set(ENABLE_SANITIZERS "OFF" CACHE STRING "Enable sanitizer: OFF|asan|tsan")
set(SANITIZER_VALID_VALUES "OFF;asan;tsan")
if(NOT ENABLE_SANITIZERS IN_LIST SANITIZER_VALID_VALUES)
    message(FATAL_ERROR "ENABLE_SANITIZERS must be one of: ${SANITIZER_VALID_VALUES} (got: ${ENABLE_SANITIZERS})")
endif()

if(ENABLE_SANITIZERS STREQUAL "asan")
    message(STATUS "Sanitizer: AddressSanitizer + UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(ENABLE_SANITIZERS STREQUAL "tsan")
    message(STATUS "Sanitizer: ThreadSanitizer")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
else()
    message(STATUS "Sanitizer: none (shipping configuration)")
endif()

# Compiler warnings and security hardening flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_compile_options(-fstack-protector-strong)
        # Explicit linker hardening: PIE, full RELRO, immediate symbol resolution
        add_compile_options(-fPIE)
        add_link_options(-pie -Wl,-z,relro,-z,now)

        # Verify glibc >= 2.33 before enabling _FORTIFY_SOURCE=3
        include(CheckCXXSourceCompiles)
        check_cxx_source_compiles("
            #include <features.h>
            #if !defined(__GLIBC__) || (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 33))
            #error \"glibc >= 2.33 required for _FORTIFY_SOURCE=3\"
            #endif
            int main() { return 0; }
        " GLIBC_SUPPORTS_FORTIFY_3)

        if(GLIBC_SUPPORTS_FORTIFY_3)
            # _FORTIFY_SOURCE requires compiler optimization enabled (-O1, -O2, -O3, -Os)
            # Only enable in optimized Release-like configurations
            add_compile_options(
                $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE>
                $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-D_FORTIFY_SOURCE=3>
            )
        endif()
    endif()
    if(UNIX AND NOT APPLE)
        add_compile_definitions(_GLIBCXX_ASSERTIONS)
    endif()
endif()

# Qt MOC / UIC / RCC automation
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Optional Windows fallback if CMAKE_PREFIX_PATH is not set by user environment
if(WIN32 AND NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH} AND NOT DEFINED ENV{Qt6_DIR})
    if(EXISTS "C:/Qt/6.7.2/mingw_64")
        list(PREPEND CMAKE_PREFIX_PATH "C:/Qt/6.7.2/mingw_64")
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport)

# Fetch prebuilt PDFium binaries with cryptographic SHA256 hash verification
include(FetchContent)

set(PDFIUM_TAG "chromium/8100")
if(WIN32)
    set(PDFIUM_ARCHIVE "pdfium-win-x64.tgz")
    set(PDFIUM_HASH "51621bcc1d9be9ea448f4b84b0516862b928e3968d3fca899af1a6d6e2848d6d")
elseif(APPLE)
    message(FATAL_ERROR "macOS is not currently supported in this Linux-focused workstation build.")
else()
    set(PDFIUM_ARCHIVE "pdfium-linux-x64.tgz")
    set(PDFIUM_HASH "1c1894a12d4d96ee01400c3874444126254180c5c7d2ab628576b67f8120320a")
endif()

set(PDFIUM_DOWNLOAD_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_TAG}/${PDFIUM_ARCHIVE}")

message(STATUS "Configuring PDFium from ${PDFIUM_DOWNLOAD_URL}")

if(NOT FETCHCONTENT_SOURCE_DIR_PDFIUM_BINARIES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pdfium_extracted/lib")
    set(FETCHCONTENT_SOURCE_DIR_PDFIUM_BINARIES "${CMAKE_CURRENT_SOURCE_DIR}/pdfium_extracted" CACHE PATH "" FORCE)
endif()

FetchContent_Declare(
    pdfium_binaries
    URL ${PDFIUM_DOWNLOAD_URL}
    URL_HASH SHA256=${PDFIUM_HASH}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(pdfium_binaries)

# Define imported target for PDFium
add_library(pdfium SHARED IMPORTED GLOBAL)
set_target_properties(pdfium PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${pdfium_binaries_SOURCE_DIR}/include"
)

if(WIN32)
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
        IMPORTED_IMPLIB "${pdfium_binaries_SOURCE_DIR}/lib/pdfium.dll.lib"
    )
else()
    set_target_properties(pdfium PROPERTIES
        IMPORTED_LOCATION "${pdfium_binaries_SOURCE_DIR}/lib/libpdfium.so"
    )
endif()

# Standalone Layout library (Pure C++20, 0 Qt, 0 PDFium dependencies)
add_library(pdf_layout STATIC
    include/layout/DocumentLayout.h
    src/layout/DocumentLayout.cpp
)

target_include_directories(pdf_layout PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Standalone in-memory Tile Cache library (Pure C++20, 0 Qt, 0 PDFium dependencies)
add_library(pdf_cache STATIC
    include/cache/TileCache.h
    src/cache/TileCache.cpp
)

target_include_directories(pdf_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Burn-in redaction engine (see docs/REDACTION.md). Pure C++20, PDFium only: no Qt, no UI.
add_library(pdf_redact STATIC
    include/redact/Redaction.h
    src/redact/Redaction.cpp
)

target_include_directories(pdf_redact PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pdf_redact PUBLIC
    pdfium
)

# Standalone IPC library for protocol messages and serialization
add_library(pdf_ipc STATIC
    include/ipc/IpcProtocol.h
    include/ipc/IpcMessages.h
    src/ipc/IpcMessages.cpp
    src/ipc/WorkerClient.h
    src/ipc/WorkerClient.cpp
    include/engine/EditLog.h
    src/engine/EditLog.cpp
)

target_include_directories(pdf_ipc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pdf_ipc PUBLIC
    pdf_layout
    pdf_cache
    Qt6::Core
    Qt6::Network
)


# Core engine library
add_library(pdf_engine STATIC
    include/engine/IDocument.h
    include/engine/IPage.h
    include/engine/PixelBuffer.h
    include/engine/PageText.h
    include/engine/SearchTypes.h
    include/engine/OutlineTypes.h
    include/engine/RenderWorker.h
    src/engine/SearchText.h
    src/engine/SearchText.cpp
    src/engine/PdfiumDocument.h
    src/engine/PdfiumDocument.cpp
    src/engine/PdfiumPage.h
    src/engine/PdfiumPage.cpp
    src/engine/RenderWorker.cpp
)

target_include_directories(pdf_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pdf_engine PUBLIC
    pdf_layout
    pdf_cache
    pdf_redact
    pdf_ipc
    Qt6::Core
    Qt6::Gui
    pdfium
)

# Isolated PDFium rendering engine worker process
add_executable(pdf_worker
    src/worker/main.cpp
    src/worker/WorkerServer.h
    src/worker/WorkerServer.cpp
)

target_include_directories(pdf_worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pdf_worker PRIVATE
    pdf_ipc
    pdf_engine
    Qt6::Core
    Qt6::Network
)

option(ENABLE_TEST_HOOKS "Enable test-only hooks in worker (fault injection). OFF for shipping builds." OFF)
option(INSTALL_SANDBOX_SELFTEST "Also install test_worker_ipc and its fixtures so the confinement tests can run inside a package sandbox (Flatpak). OFF for shipping builds." OFF)
if(ENABLE_TEST_HOOKS)
    message(WARNING "ENABLE_TEST_HOOKS=ON: worker fault-injection hooks are compiled in. Do not ship this build.")
    target_compile_definitions(pdf_worker PRIVATE PDF_WORKER_TEST_HOOKS=1)
    target_compile_definitions(pdf_engine PUBLIC PDF_WORKER_TEST_HOOKS=1)
    target_compile_definitions(pdf_ipc PUBLIC PDF_WORKER_TEST_HOOKS=1)
else()
    message(STATUS "ENABLE_TEST_HOOKS=OFF: shipping worker configuration.")
endif()

# Main desktop application executable
add_executable(pdf-reader
    main.cpp
    src/ui/ColorMode.h
    src/ui/ImageUtils.h
    src/ui/FindBar.h
    src/ui/FindBar.cpp
    src/ui/ThumbnailListWidget.h
    src/ui/ThumbnailListWidget.cpp
    src/ui/SearchResultsWidget.h
    src/ui/SearchResultsWidget.cpp
    src/ui/PdfViewerWidget.h
    src/ui/PdfViewerWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

target_include_directories(pdf-reader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pdf-reader PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    pdf_ipc
)
add_dependencies(pdf-reader pdf_worker)

# Backward-compatibility alias for CLion workspace
add_executable(PDf_Reader_for_Linux ALIAS pdf-reader)

# Test executables for headless automated verification
add_executable(test_render
    test_render.cpp
)

target_link_libraries(test_render PRIVATE
    pdf_engine
)

# Standalone layout test (links solely to pdf_layout, 0 Qt, 0 PDFium)
add_executable(test_layout
    test_layout.cpp
)

target_link_libraries(test_layout PRIVATE
    pdf_layout
)

# Standalone tile cache test (links solely to pdf_cache, 0 Qt, 0 PDFium)
add_executable(test_tile_cache
    test_tile_cache.cpp
)

target_link_libraries(test_tile_cache PRIVATE
    pdf_cache
)

# Viewport Widget headless test (subclasses QAbstractScrollArea)
add_executable(test_widget
    test_widget.cpp
    src/ui/ColorMode.h
    src/ui/ImageUtils.h
    src/ui/FindBar.h
    src/ui/FindBar.cpp
    src/ui/ThumbnailListWidget.h
    src/ui/ThumbnailListWidget.cpp
    src/ui/SearchResultsWidget.h
    src/ui/SearchResultsWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/PdfViewerWidget.cpp
)

target_include_directories(test_widget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
)

target_link_libraries(test_widget PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    pdf_engine
    pdf_ipc
)
if(ENABLE_TEST_HOOKS)
    target_compile_definitions(test_widget PRIVATE PDF_WORKER_TEST_HOOKS=1)
endif()

# Pixel-level end-to-end test: real widget + real sandboxed worker + pdftoppm witness.
add_executable(test_redaction_rotation
    tests/ui/test_redaction_rotation.cpp
    src/ui/ColorMode.h
    src/ui/FindBar.h
    src/ui/FindBar.cpp
    src/ui/ThumbnailListWidget.h
    src/ui/ThumbnailListWidget.cpp
    src/ui/SearchResultsWidget.h
    src/ui/SearchResultsWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/PdfViewerWidget.cpp
)
target_include_directories(test_redaction_rotation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
)
target_link_libraries(test_redaction_rotation PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    pdf_engine
    pdf_ipc
)

# Unit tests for the redaction save guards (never overwrite the source; atomic all-or-nothing write).
add_executable(test_redaction_file_safety
    tests/ui/test_redaction_file_safety.cpp
    src/ui/RedactionFileSafety.h
)
target_include_directories(test_redaction_file_safety PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_redaction_file_safety PRIVATE Qt6::Core)

# Headless tests for the encrypted-PDF password prompt workflow.
add_executable(test_password_prompt
    test_password_prompt.cpp
    src/ui/FindBar.h
    src/ui/FindBar.cpp
    src/ui/ThumbnailListWidget.h
    src/ui/ThumbnailListWidget.cpp
    src/ui/SearchResultsWidget.h
    src/ui/SearchResultsWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/PdfViewerWidget.cpp
)

target_include_directories(test_password_prompt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
)

target_link_libraries(test_password_prompt PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    pdf_engine
    pdf_ipc
)

# Unit test for worker IPC, lifecycle, and process isolation
add_executable(test_worker_ipc
    test_worker_ipc.cpp
)
if(ENABLE_TEST_HOOKS)
    target_compile_definitions(test_worker_ipc PRIVATE PDF_WORKER_TEST_HOOKS=1)
endif()

target_include_directories(test_worker_ipc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(test_worker_ipc PRIVATE
    pdf_ipc
    Qt6::Core
    Qt6::Network
)

# IPC message deserialization fuzz test
add_executable(fuzz_ipc_messages
    tests/fuzz/fuzz_ipc_messages.cpp
)
target_include_directories(fuzz_ipc_messages PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(fuzz_ipc_messages PRIVATE
    pdf_ipc
    Qt6::Core
    Qt6::Network
)

# Redaction engine tests (need zlib for the independent inspector's inflate; skipped when absent)
find_package(ZLIB QUIET)
if(ZLIB_FOUND AND UNIX)
    add_executable(test_redaction tests/redaction/test_redaction.cpp tests/redaction/PdfInspector.h)
    target_link_libraries(test_redaction PRIVATE pdf_redact ZLIB::ZLIB)
    target_include_directories(test_redaction PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/redaction)
else()
    message(STATUS "zlib not found (or non-Unix): test_redaction is not built")
endif()

# CTest registration
enable_testing()
add_test(
    NAME EngineRenderTestSuite
    COMMAND test_render "${CMAKE_CURRENT_SOURCE_DIR}/sample.pdf"
)

add_test(
    NAME DocumentLayoutTestSuite
    COMMAND test_layout
)

add_test(
    NAME TileCacheTestSuite
    COMMAND test_tile_cache
)

add_test(
    NAME ViewportWidgetTestSuite
    COMMAND test_widget "${CMAKE_CURRENT_SOURCE_DIR}/sample.pdf"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/redaction/signed.pdf"
)

add_test(
    NAME PasswordPromptTestSuite
    COMMAND test_password_prompt
        "${CMAKE_CURRENT_SOURCE_DIR}/sample.pdf"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/encrypted.pdf"
)

add_test(
    NAME RedactionRotationTestSuite
    COMMAND test_redaction_rotation "${CMAKE_CURRENT_SOURCE_DIR}/sample.pdf"
)
add_test(
    NAME RedactionFileSafetyTestSuite
    COMMAND test_redaction_file_safety
)
add_test(
    NAME WorkerIpcTestSuite
    COMMAND test_worker_ipc "$<TARGET_FILE:pdf_worker>" "${CMAKE_CURRENT_SOURCE_DIR}/sample.pdf"
)

if(TARGET test_redaction)
    add_test(
        NAME RedactionTestSuite
        COMMAND test_redaction "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/redaction"
    )
endif()

add_test(
    NAME IpcFuzzTestSuite
    COMMAND fuzz_ipc_messages
)

if(WIN32)
    get_filename_component(COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(QT_BIN_DIR "${Qt6Core_DIR}/../../../bin" ABSOLUTE)
    get_filename_component(QT_PLUGIN_DIR "${Qt6Core_DIR}/../../../plugins" ABSOLUTE)
    set_tests_properties(EngineRenderTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR}"
    )
    # test_layout and test_tile_cache have 0 external runtime dependencies, but compiler bin prepend is safe
    set_tests_properties(DocumentLayoutTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${COMPILER_BIN_DIR}"
    )
    set_tests_properties(TileCacheTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${COMPILER_BIN_DIR}"
    )
    set_tests_properties(ViewportWidgetTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR};QT_PLUGIN_PATH=set:${QT_PLUGIN_DIR};QT_QPA_PLATFORM=set:offscreen"
    )
    set_tests_properties(PasswordPromptTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR};QT_PLUGIN_PATH=set:${QT_PLUGIN_DIR};QT_QPA_PLATFORM=set:offscreen"
    )
    set_tests_properties(RedactionRotationTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR};QT_PLUGIN_PATH=set:${QT_PLUGIN_DIR};QT_QPA_PLATFORM=set:offscreen"
    )
    set_tests_properties(RedactionFileSafetyTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR}"
    )
    set_tests_properties(WorkerIpcTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR};QT_PLUGIN_PATH=set:${QT_PLUGIN_DIR};QT_QPA_PLATFORM=set:offscreen"
    )
    set_tests_properties(IpcFuzzTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR}"
    )
endif()

# Scoped workaround specifically for Windows MinGW GCC >= 15 toolchain (CWG 2387 bug in Qt 6.7.2 qcomparehelpers.h)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15.0")
    target_link_options(pdf-reader PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_render PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_widget PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_password_prompt PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(pdf_worker PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_worker_ipc PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(fuzz_ipc_messages PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_redaction_rotation PRIVATE -Wl,--allow-multiple-definition)
endif()

# Deployment & RPATH handling
if(WIN32)
    add_custom_command(TARGET test_render POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:test_render>"
        COMMENT "Deploying pdfium.dll next to test_render"
    )
    add_custom_command(TARGET test_widget POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:test_widget>"
        COMMENT "Deploying pdfium.dll next to test_widget"
    )
    add_custom_command(TARGET test_password_prompt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:test_password_prompt>"
        COMMENT "Deploying pdfium.dll next to test_password_prompt"
    )
    add_custom_command(TARGET test_redaction_rotation POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:test_redaction_rotation>"
        COMMENT "Deploying pdfium.dll next to test_redaction_rotation"
    )
    add_custom_command(TARGET pdf_worker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:pdf_worker>"
        COMMENT "Deploying pdfium.dll next to pdf_worker"
    )
elseif(UNIX AND NOT APPLE)
    set_target_properties(test_render PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    set_target_properties(test_widget PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    set_target_properties(test_password_prompt PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    set_target_properties(test_redaction_rotation PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    set_target_properties(pdf_worker PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

# Installation & Packaging
install(TARGETS pdf-reader pdf_worker
    RUNTIME DESTINATION bin
)
if(UNIX AND NOT APPLE)
    install(FILES resources/org.greenware.PDFReader.desktop
        DESTINATION share/applications
        OPTIONAL
    )
    install(FILES resources/pdf-reader.desktop
        DESTINATION share/applications
        OPTIONAL
    )
    install(FILES resources/org.greenware.PDFReader.metainfo.xml
        DESTINATION share/metainfo
        OPTIONAL
    )
    install(FILES resources/org.greenware.PDFReader.svg
        DESTINATION share/icons/hicolor/scalable/apps
        OPTIONAL
    )
    if(EXISTS "${pdfium_binaries_SOURCE_DIR}/lib/libpdfium.so")
        install(FILES "${pdfium_binaries_SOURCE_DIR}/lib/libpdfium.so"
            DESTINATION lib
        )
    endif()
    if(INSTALL_SANDBOX_SELFTEST)
        # scripts/test_flatpak_sandbox.sh --test-runtime runs these inside the sandbox. Never enabled in shipping manifests.
        message(WARNING "INSTALL_SANDBOX_SELFTEST=ON: the test binary will be installed. Do not ship this build.")
        # Next to pdf_worker: WorkerClient looks for the worker in QCoreApplication::applicationDirPath().
        install(TARGETS test_worker_ipc RUNTIME DESTINATION bin)
        install(FILES sample.pdf
                      tests/fixtures/unicode_embedded.pdf
                      tests/fixtures/unicode_system_font.pdf
                DESTINATION share/pdf-reader/selftest)
    endif()
endif()
