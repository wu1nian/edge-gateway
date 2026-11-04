@echo off
REM Build the ring_buffer + net_fsm PC unit test.
REM Requires gcc on PATH (MinGW or WinLibs both work).
REM NOTE: keep this file ASCII-only. A .bat with UTF-8 Chinese comments can be
REM mis-parsed by cmd.exe (it splits on bytes and leaves fragments behind),
REM which makes the build print bogus "not recognized" errors on stderr.

setlocal
cd /d "%~dp0"

set SRCDIR=..\..\firmware\EdgeGateway\Core
set CFLAGS=-std=c99 -Wall -Wextra -Werror -O2

del /q test_ring_fsm.exe 2>nul

gcc %CFLAGS% -I"%SRCDIR%\Inc" test_ring_fsm.c "%SRCDIR%\Src\ring_buffer.c" "%SRCDIR%\Src\net_fsm.c" -o test_ring_fsm.exe
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

if not exist test_ring_fsm.exe (
    echo BUILD FAILED
    exit /b 1
)

echo BUILD OK
endlocal