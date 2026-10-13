cmake_minimum_required(VERSION 3.24)
project(PDf_Reader_for_Linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt MOC / UIC / RCC automation
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Autodetect installed Qt 6 MinGW on Windows if present
if(WIN32 AND EXISTS "C:/Qt/6.7.2/mingw_64")
    list(PREPEND CMAKE_PREFIX_PATH "C:/Qt/6.7.2/mingw_64")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Fetch prebuilt PDFium binaries (bblanchon/pdfium-binaries)
include(FetchContent)

set(PDFIUM_TAG "chromium/8100")
if(WIN32)
    set(PDFIUM_ARCHIVE "pdfium-win-x64.tgz")
elseif(APPLE)
    set(PDFIUM_ARCHIVE "pdfium-mac-x64.tgz")
else()
    set(PDFIUM_ARCHIVE "pdfium-linux-x64.tgz")
endif()

set(PDFIUM_DOWNLOAD_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_TAG}/${PDFIUM_ARCHIVE}")

message(STATUS "Configuring PDFium from ${PDFIUM_DOWNLOAD_URL}")

FetchContent_Declare(
    pdfium_binaries
    URL ${PDFIUM_DOWNLOAD_URL}
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

# Core engine library
add_library(pdf_engine STATIC
    include/engine/IDocument.h
    include/engine/IPage.h
    src/engine/PdfiumDocument.h
    src/engine/PdfiumDocument.cpp
    src/engine/PdfiumPage.h
    src/engine/PdfiumPage.cpp
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

# Main GUI application
add_executable(PDf_Reader_for_Linux
    main.cpp
    src/ui/PdfViewerWidget.h
    src/ui/PdfViewerWidget.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

target_include_directories(PDf_Reader_for_Linux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(PDf_Reader_for_Linux PRIVATE
    Qt6::Widgets
    pdf_engine
)

# Test executable for headless automated verification
add_executable(test_render
    test_render.cpp
)

target_link_libraries(test_render PRIVATE
    pdf_engine
)

# GCC 15+ conformance workaround for Qt 6.7.2 qcomparehelpers.h missing inline on IsFloatType_v
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "15.0")
    target_link_options(PDf_Reader_for_Linux PRIVATE -Wl,--allow-multiple-definition)
    target_link_options(test_render PRIVATE -Wl,--allow-multiple-definition)
endif()

# Deployment helper: ensure pdfium shared library is accessible at runtime
if(WIN32)
    add_custom_command(TARGET PDf_Reader_for_Linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:PDf_Reader_for_Linux>"
        COMMENT "Deploying pdfium.dll next to executable"
    )
    add_custom_command(TARGET test_render POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${pdfium_binaries_SOURCE_DIR}/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:test_render>"
        COMMENT "Deploying pdfium.dll next to test_render"
    )
elseif(UNIX AND NOT APPLE)
    set_target_properties(PDf_Reader_for_Linux PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
    )
    set_target_properties(test_render PROPERTIES
        BUILD_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
        INSTALL_RPATH "${pdfium_binaries_SOURCE_DIR}/lib"
    )
endif()
