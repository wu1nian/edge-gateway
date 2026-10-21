# edge-gateway · 工业物联网边缘网关（复刻优化）

本目录是 **STM32F411 + FreeRTOS + LVGL 工业边缘网关** 的学习/求职项目工作区。

参考项目：[`zgd-dev/-V2.0`](https://github.com/zgd-dev/-V2.0)（commit `40b4460`）

> ⚠️ 本目录**不直接修改参考仓库**。从 CubeMX 新建工程自己重写，写完一个模块再对照参考找差距。

## 目录结构

```
edge-gateway/
├── reference/zgd-V2.0/   原仓库克隆（只读参考，含完整提交历史）
├── firmware/             你自己的工程（CubeMX 生成，从零新建）
├── docs/                 设计文档：任务架构、Flash 分区表、抓包记录、压测报告
└── hardware/             BOM、接线图、引脚分配表
```

## 硬件（约 ¥130）

STM32F411CEU6 · ESP8266(AT固件) · W25Q64 · 2.8" SPI 触摸屏(ILI9341+XPT2046) · SP3485 · ST-Link V2

## 当前进度（2026-10-21）

- **W1 已完成**：CubeMX 骨架、96 MHz 时钟树、FreeRTOS（CMSIS-RTOS v2）、printf 重定向到 USART1。
- **W2 主体完成**：手写 Modbus-RTU 主站——CRC-16 查表、0x03/0x04/0x06 组帧、正常/异常响应解析、双层超时；STM32 侧接 USART2 + RS485 方向脚；PC 侧用 com0com + Modbus Slave 把真串口链路跑通，SHT20 温湿度帧已经对上了。
- **接下来**：等硬件到货上板，用逻辑分析仪抓 RS485 波形，把 W2 的联调验收收尾，再进 W3。

> 验证边界：协议逻辑目前只在 PC 白盒测试和虚拟串口上验证过；RS485 实际电平、方向切换时序、传感器真实读数，还要等板子和传感器到手后确认。

## 文档与验证

- 协议合同（寄存器表、哨兵帧、手册 CRC 勘误）：[`docs/modbus.md`](docs/modbus.md)
- PC 仿真怎么跑（COM3/COM4、Modbus Slave 配置、错误注入）：[`tools/pc_sim/README.md`](tools/pc_sim/README.md)
