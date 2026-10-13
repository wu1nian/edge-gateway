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
