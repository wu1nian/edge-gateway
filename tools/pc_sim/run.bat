@echo off
setlocal
cd /d "%~dp0"
pc_sim.exe --com COM4 --func 4 --addr 1 --reg 1 --count 2 %*
