@echo off
REM 编译 ring_buffer.c + net_fsm.c 的 PC 单元测试
REM 需要 gcc 在 PATH 里（MinGW / WinLibs 都行）

setlocal
cd /d "%~dp0"

set SRCDIR=..\..\firmware\EdgeGateway\Core
set CFLAGS=-std=c99 -Wall -Wextra -Werror -O2

del /q test_ring_fsm.exe 2>nul

gcc %CFLAGS% -I"%SRCDIR%\Inc" test_ring_fsm.c "%SRCDIR%\Src\ring_buffer.c" "%SRCDIR%\Src\net_fsm.c" -o test_ring_fsm.exe

if not exist test_ring_fsm.exe (
    echo BUILD FAILED
    exit /b 1
)

echo BUILD OK
endlocal