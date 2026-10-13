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

## 三周计划与踩坑笔记

详细计划、原项目坑位清单在 Obsidian 中：

- `嵌入式笔记/边缘网关项目/00-边缘网关项目总览与三周计划.md`
- `嵌入式笔记/边缘网关项目/01-原项目坑位清单与优化项.md`

## 关键约束（来自原项目审查）

动手前先看一眼这几条，别把原项目的坑抄进来：

1. CubeMX 里 Debug 必须设成 **Serial Wire**，别用 PB3/PB4/PA15 当普通 IO
2. 禁用 `sprintf`，一律 `snprintf`
3. MQTT 报文封装参考 `libemqtt`（LGPL 开源库），README 里**如实标注**，别写"从零手写"
4. 每个 `while(1)` 任务的节拍必须有明确来源，不留死代码
5. Flash 访问一开始就加互斥锁
6. 任务栈用 `uxTaskGetStackHighWaterMark()` 实测后确定

## 硬件（约 ¥130）

STM32F411CEU6 · ESP8266(AT固件) · W25Q64 · 2.8" SPI 触摸屏(ILI9341+XPT2046) · SP3485 · ST-Link V2
