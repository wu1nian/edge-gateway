@echo off
rem Usage:
rem   run.bat                          -> 127.0.0.1:1883 (local Mosquitto)
rem   run.bat broker.emqx.io 1883      -> public broker fallback
rem   run.bat broker.emqx.io 1883 --positive-only
setlocal
cd /d "%~dp0"

if "%~1"=="" (
    echo [info] no host given, using 127.0.0.1:1883
    echo [info] public fallback: run.bat broker.emqx.io 1883
    .\broker_check.exe 127.0.0.1 1883
) else (
    .\broker_check.exe %*
)

exit /b %errorlevel%