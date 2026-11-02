# edge-gateway · 工业物联网边缘网关（复刻优化）

本目录是 **STM32F411 + FreeRTOS + LVGL 工业边缘网关** 的学习/求职项目工作区。

参考项目：[`zgd-dev/-V2.0`](https://github.com/zgd-dev/-V2.0)（commit `40b4460`）

> ⚠️ 本目录**不直接修改参考仓库**。从 CubeMX 新建工程自己重写，写完一个模块再对照参考找差距。

## 目录结构

```
edge-gateway/
├── reference/zgd-V2.0/   原仓库克隆（只读参考，含完整提交历史）
├── firmware/             你自己的工程（CubeMX 生成，从零新建）
├── docs/                 设计文档：协议合同、验证记录、阶段总结
├── tools/                PC 侧验证工具：Modbus 仿真 / MQTT 单元测试 / 真 broker 检查
└── hardware/             BOM、接线图、引脚分配表
```

## 硬件（约 ¥130）

STM32F411CEU6 · ESP8266(AT固件) · W25Q64 · 2.8" SPI 触摸屏(ILI9341+XPT2046) · SP3485 · ST-Link V2

## 当前进度（2026-10-31）

| 周 | 主题 | 状态 |
|:---:|:---|:---|
| W1 | 环境与骨架（CubeMX / 时钟树 / FreeRTOS / printf 重定向） | ✅ 完成 |
| W2 | 手写 Modbus-RTU 主站 + PC 仿真 | ✅ 主体完成 |
| **W3** | **手写 MQTT 3.1.1 报文层 + 真 broker 验证** | **✅ 完成** |
| W4 | RTOS 任务架构（6 任务 / 锁与队列 / 数据流） | 🚧 进行中 |
| W5–W9 | Flash 分区 · 采集与 LVGL · ESP8266 上云 · 优化压测 · 收尾 | ⬜ 未开始 |

- **W1 已完成**：CubeMX 骨架、96 MHz 时钟树、FreeRTOS（CMSIS-RTOS v2）、printf 重定向到 USART1。
- **W2 主体完成**：手写 Modbus-RTU 主站——CRC-16 查表、0x03/0x04/0x06 组帧、正常/异常响应解析、双层超时；STM32 侧接 USART2 + RS485 方向脚；PC 侧用 com0com + Modbus Slave 把真串口链路跑通，SHT20 温湿度帧已经对上了。
- **W3 已完成**：**不用任何 MQTT 客户端库**，按 OASIS 3.1.1 规范从零手写报文层（`mqtt_codec.c` 692 行，纯 C、不依赖 HAL/FreeRTOS）——Remaining Length 编解码、CONNECT / SUBSCRIBE / PUBLISH / PUBACK / PINGREQ / DISCONNECT 组帧、收包分类、心跳到期判断。
  - 单元测试 **29 / 29 PASS**（`tools/mqtt_test/`，`-Wall -Wextra -Werror` 零警告）；
  - 真 broker 正向 **7 项 + 反例 4 条全部通过**：本机 Mosquitto 2.1.2、公网 HiveMQ（连测 5 次）、公网 EMQX 三家独立实现结论一致；
  - 固件编译 0 error / 0 warning，RAM 16.47% / FLASH 7.90%；
  - 详见 [`docs/w3-summary.md`](docs/w3-summary.md)（阶段总结）与 [`docs/mqtt.md`](docs/mqtt.md)（验证记录 + 自写/参考边界）。
- **接下来**：W4 任务架构——把 W2 的 Modbus 与 W3 的 MQTT 接进 6 个任务的优先级 / 队列 / 锁体系。

> **验证边界**（重要）：本周 broker 验证全部在 **PC 上**完成——Windows gcc 编译 `mqtt_codec.c`、Winsock 直连 broker。
> 这证明**协议实现正确**，但**尚未证明** STM32 侧真实收发、ESP8266 AT 透传、巴法云实连——
> 这三项要等硬件到货后在 W7 补。RS485 实际电平、方向切换时序、传感器真实读数同样待硬件确认。
> **编译通过 ≠ 跑通。**

## 文档与验证

- **W3 阶段总结**：[`docs/w3-summary.md`](docs/w3-summary.md)
- MQTT 验证记录 + 自写/参考边界：[`docs/mqtt.md`](docs/mqtt.md)
- 真 broker 怎么跑（Mosquitto 配置、反例注入、公网 broker）：[`tools/mqtt_broker_check/README.md`](tools/mqtt_broker_check/README.md)
- 协议合同（寄存器表、哨兵帧、手册 CRC 勘误）：[`docs/modbus.md`](docs/modbus.md)
- PC 仿真怎么跑（COM3/COM4、Modbus Slave 配置、错误注入）：[`tools/pc_sim/README.md`](tools/pc_sim/README.md)
