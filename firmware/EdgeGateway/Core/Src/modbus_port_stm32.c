#include "modbus_port_stm32.h"
#include "usart.h"   /* huart2：RS485 串口（USART2，9600 8N1） */
#include "main.h"    /* RS485_DE_Pin / RS485_DE_GPIO_Port */

/* 清空接收寄存器里的残留字节：
 * 上一次通信的噪声/半帧如果留在 DR 里，会被当成本次响应的首字节。 */
static void uart_flush_rx(UART_HandleTypeDef *h)
{
    volatile uint32_t tmp;

    while (__HAL_UART_GET_FLAG(h, UART_FLAG_RXNE) != RESET)
    {
        tmp = h->Instance->DR;
    }
    __HAL_UART_CLEAR_OREFLAG(h);
    (void)tmp;
}

/**
 * @brief 通过 USART2 发送一帧，并控制 RS485 收发方向。
 * @param buf 待发送数据
 * @param len 待发送字节数
 * @param ctx 平台上下文，这里是 UART_HandleTypeDef*
 * @return 0=成功，-1=失败
 */
static int uart_write(const uint8_t *buf, uint16_t len, void *ctx)
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;
    HAL_StatusTypeDef st;

    uart_flush_rx(h);                                                   /* 先清残留 */
    HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_SET);  /* DE=1：切到发送 */
    st = HAL_UART_Transmit(h, (uint8_t *)buf, len, 100);                /* 8 字节 @9600 约 8.3ms */

    if (st == HAL_OK)
    {
        /* 等最后一个字节真正移出（TC），再切回接收，避免截断帧尾 */
        while (__HAL_UART_GET_FLAG(h, UART_FLAG_TC) == RESET)
        {
        }
    }
    HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET); /* DE=0：切回接收 */

    return (st == HAL_OK) ? 0 : -1;
}

/**
 * @brief 从 USART2 阻塞接收 1 个字节。
 * @param b          输出：收到的字节
 * @param timeout_ms 最多等待的毫秒数
 * @param ctx        平台上下文，这里是 UART_HandleTypeDef*
 * @return 0=成功，-1=超时/错误
 */
static int uart_read_byte(uint8_t *b, uint32_t timeout_ms, void *ctx)
{
    UART_HandleTypeDef *h = (UART_HandleTypeDef *)ctx;
    return (HAL_UART_Receive(h, b, 1, timeout_ms) == HAL_OK) ? 0 : -1;
}

/* @brief 返回 HAL 毫秒计数，供 mb_transact 计算超时。 */
static uint32_t tick_ms(void)
{
    return HAL_GetTick();
}

/* 平台上下文：RS485 接在 USART2。 */
static const mb_port_t s_port = {
    uart_write,      /* 发送一帧 */
    uart_read_byte,  /* 收 1 字节 */
    tick_ms,         /* 毫秒计时 */
    &huart2          /* 传给上面三个回调的 ctx */
};

const mb_port_t *mb_port_stm32(void)
{
    return &s_port;
}
