@echo off
rem S20 real-broker check: Winsock client + the same mqtt_codec used on STM32.
rem Output broker_check.exe is ignored by the repo's *.exe gitignore rule.
setlocal
cd /d "%~dp0"

gcc -std=c11 -Wall -Wextra -Werror -O2 ^
    -I"..\..\firmware\EdgeGateway\Core\Inc" ^
    "..\..\firmware\EdgeGateway\Core\Src\mqtt_codec.c" ^
    broker_check.c -o broker_check.exe -lws2_32

if errorlevel 1 (
    echo Build FAILED
    exit /b 1
)
echo Build OK: broker_check.exe