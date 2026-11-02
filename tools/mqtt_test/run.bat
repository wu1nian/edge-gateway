@echo off
rem Run S15-S19 codec self-test; exit code 0 means all vectors passed.
setlocal
cd /d "%~dp0"
.\test_mqtt_codec.exe