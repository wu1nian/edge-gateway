#ifndef MODBUS_PORT_STM32_H
#define MODBUS_PORT_STM32_H

#include "modbus_host.h"

/**
 * @brief 取得接在 STM32 HAL 上的 Modbus 移植层（USART2 + RS485_DE 方向控制）。
 * @return 指向静态 mb_port_t 的只读指针，可直接传给 mb_transact()
 */
const mb_port_t *mb_port_stm32(void);

#endif /* MODBUS_PORT_STM32_H */
