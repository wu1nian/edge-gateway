@echo off
setlocal
cd /d "%~dp0"
rem 与笔记 14 的从站定义一致：Slave ID=1, Function=03, Address=0, Quantity=2
pc_sim.exe --com COM4 --func 3 --addr 1 --reg 0 --count 2 %*
