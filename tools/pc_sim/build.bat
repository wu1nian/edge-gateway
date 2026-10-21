@echo off
setlocal
cd /d "%~dp0"

gcc -Wall -Wextra -O2 -I"..\..\firmware\EdgeGateway\Core\Inc" ^
    "..\..\firmware\EdgeGateway\Core\Src\modbus_host.c" ^
    win_serial_port.c pc_sim_main.c -o pc_sim.exe

if errorlevel 1 (
    echo Build FAILED
    exit /b 1
)
echo Build OK: pc_sim.exe
