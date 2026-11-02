@echo off
rem S15-S19 codec self-test: build pure protocol layer + test main with MinGW gcc.
rem Output test_mqtt_codec.exe is ignored by the repo's *.exe gitignore rule.
setlocal
cd /d "%~dp0"

gcc -std=c11 -Wall -Wextra -Werror -O2 ^
    -I"..\..\firmware\EdgeGateway\Core\Inc" ^
    "..\..\firmware\EdgeGateway\Core\Src\mqtt_codec.c" ^
    test_mqtt_codec.c -o test_mqtt_codec.exe

if errorlevel 1 (
    echo Build FAILED
    exit /b 1
)
echo Build OK: test_mqtt_codec.exe