@echo off
set "PATH=C:\Program Files\JetBrains\CLion 2026.2.3.1\bin\mingw\bin;C:\Qt\6.7.2\mingw_64\bin;%PATH%"
cd /d "%~dp0cmake-build-debug"
test_render.exe "..\sample.pdf"
echo Exit code: %ERRORLEVEL%
