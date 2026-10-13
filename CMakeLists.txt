cmake_minimum_required(VERSION 3.24)
project(pdf-reader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler warnings and security hardening flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(UNIX AND NOT APPLE)
        add_compile_definitions(_GLIBCXX_ASSERTIONS)
    endif()
endif()

# Qt MOC / UIC / RCC automation
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Autodetect installed Qt 6 MinGW on Windows if present
if(WIN32 AND EXISTS "C:/Qt/6.7.2/mingw_64")
    list(PREPEND CMAKE_PREFIX_PATH "C:/Qt/6.7.2/mingw_64")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

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

# Core engine and layout library
add_library(pdf_engine STATIC
    include/engine/IDocument.h
    include/engine/IPage.h
    include/engine/PixelBuffer.h
    include/layout/DocumentLayout.h
    src/engine/PdfiumDocument.h
    src/engine/PdfiumDocument.cpp
    src/engine/PdfiumPage.h
    src/engine/PdfiumPage.cpp
    src/layout/DocumentLayout.cpp
)

target_include_directories(pdf_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pdf_engine PUBLIC
    Qt6::Core
    Qt6::Gui
    pdfium
)

# Main desktop application executable
add_executable(pdf-reader
    main.cpp
    src/ui/ImageUtils.h
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
    pdf_engine
)

# Backward-compatibility alias for CLion workspace
add_executable(PDf_Reader_for_Linux ALIAS pdf-reader)

# Test executables for headless automated verification
add_executable(test_render
    test_render.cpp
)

target_link_libraries(test_render PRIVATE
    pdf_engine
)

add_executable(test_layout
    test_layout.cpp
)

target_link_libraries(test_layout PRIVATE
    pdf_engine
)

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

if(WIN32)
    get_filename_component(COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(QT_BIN_DIR "${Qt6Core_DIR}/../../../bin" ABSOLUTE)
    set_tests_properties(EngineRenderTestSuite DocumentLayoutTestSuite PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_BIN_DIR};PATH=path_list_prepend:${COMPILER_BIN_DIR}"
    )
endif()

# Scoped workaround specifically for Windows MinGW GCC >= 15 toolchain (CWG 2387 bug in Qt 6.7.2 qcomparehelpers.h)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15.0")
    target_link_options(pdf-reader PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_render PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_layout PRIVATE -Wl,--allow-multiple-definition)
endif()

# Deployment & RPATH handling
if(WIN32)
    add_custom_command(TARGET pdf-reader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:pdf-reader>"
        COMMENT "Deploying pdfium.dll next to pdf-reader"
    )
    add_custom_command(TARGET test_render POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:test_render>"
        COMMENT "Deploying pdfium.dll next to test_render"
    )
    add_custom_command(TARGET test_layout POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:test_layout>"
        COMMENT "Deploying pdfium.dll next to test_layout"
    )
elseif(UNIX AND NOT APPLE)
    set_target_properties(pdf-reader PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    set_target_properties(test_render PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
    set_target_properties(test_layout PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()
