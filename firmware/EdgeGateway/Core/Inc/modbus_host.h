#ifndef MODBUS_HOST_H
#define MODBUS_HOST_H

#include <stdint.h>

/* ===================== 参数范围 ===================== */
#define MB_ADDR_MIN          0x01u   /* 合法从机地址下限（0 是广播地址，主站读操作不用） */
#define MB_ADDR_MAX          0xF7u   /* 合法从机地址上限（247） */
#define MB_READ_MAX          125u    /* 0x03/0x04 单次最多读 125 个寄存器 */

/* ===================== 功能码 ===================== */
#define MB_FC_READ_HOLDING   0x03u   /* 读保持寄存器 */
#define MB_FC_READ_INPUT     0x04u   /* 读输入寄存器（SHT20 温湿度用这个） */
#define MB_FC_WRITE_SINGLE   0x06u   /* 写单个保持寄存器 */
#define MB_FC_WRITE_MULTI    0x10u   /* 写多个保持寄存器 */

/* ===================== SHT20 RS485 温湿度传感器寄存器 ===================== */
#define SHT20_REG_TEMP       0x0001u /* 温度（输入寄存器，有符号 int16，单位 0.1℃） */
#define SHT20_REG_HUMI       0x0002u /* 湿度（输入寄存器，单位 0.1%RH） */
#define SHT20_REG_ADDR       0x0101u /* 从机地址（保持寄存器） */
#define SHT20_REG_BAUD       0x0102u /* 波特率（保持寄存器） */
#define SHT20_REG_TCAL       0x0103u /* 温度校准（保持寄存器） */
#define SHT20_REG_HCAL       0x0104u /* 湿度校准（保持寄存器） */

/* ===================== 返回码 ===================== */
typedef enum {
    MB_OK = 0,          /* 成功 */
    MB_ERR_PARAM,       /* 入参非法（空指针 / 地址或数量超范围） */
    MB_ERR_LEN,         /* 帧长不对 / 接收缓冲不足 */
    MB_ERR_ADDR,        /* 从机地址与期望不符 */
    MB_ERR_FUNC,        /* 功能码与期望不符 */
    MB_ERR_CRC,         /* CRC 校验失败 */
    MB_ERR_EXCEPTION,   /* 从站回了异常帧（异常码在 out_exc 里） */
    MB_ERR_TIMEOUT,     /* 超时（一个字节都没收到） */
    MB_ERR_IO           /* 发送失败 / 串口 IO 错误 */
} mb_status_t;

/* ===================== 移植层（依赖注入） ===================== */
typedef struct {
    /* 发送一帧：buf/len 为待发数据；ctx 为平台上下文。
     * 返回 0=成功，非 0=失败。 */
    int (*write)(const uint8_t *buf, uint16_t len, void *ctx);

    /* 接收 1 个字节：b 为输出；timeout_ms 为最多等待的毫秒数。
     * 返回 0=成功，非 0=超时或错误。 */
    int (*read_byte)(uint8_t *b, uint32_t timeout_ms, void *ctx);

    /* 返回系统启动以来的毫秒数，用于超时计算。 */
    uint32_t (*tick_ms)(void);

    /* 平台私有上下文。STM32 上通常填 &huart2。 */
    void *ctx;
} mb_port_t;

/* ===================== 事务：发一帧 + 收一帧 ===================== */
/**
 * @brief 通过移植层发送请求帧，并接收应答帧（字节间静默 + 整帧双层超时）。
 * @param port      移植层函数表，不能为 NULL
 * @param req       请求帧首地址，不能为 NULL
 * @param req_len   请求帧字节数（0x03/0x04 固定为 8）
 * @param resp      应答帧接收缓冲区，不能为 NULL
 * @param resp_cap  接收缓冲区容量（字节），防止写越界
 * @param resp_len  输出：实际收到的字节数；失败时置 0
 * @return MB_OK 成功收到至少 1 字节；MB_ERR_TIMEOUT 一个字节都没收到；
 *         MB_ERR_LEN 缓冲区不足；MB_ERR_IO 发送失败；MB_ERR_PARAM 入参非法
 */
mb_status_t mb_transact(const mb_port_t *port,
                        const uint8_t *req, uint16_t req_len,
                        uint8_t *resp, uint16_t resp_cap, uint16_t *resp_len);

/* ===================== 响应帧解析 ===================== */
/**
 * @brief 解析 0x03/0x04 的响应帧（正常帧、异常帧都支持）。
 * @param frame     收到的完整帧首地址，不能为 NULL
 * @param len       帧总字节数（含末尾 2 字节 CRC）
 * @param addr      期望的从机地址（1~247）
 * @param func      期望的功能码：MB_FC_READ_HOLDING(0x03) 或 MB_FC_READ_INPUT(0x04)
 * @param out_vals  输出：解析出的寄存器值数组（每个寄存器 1 个 uint16_t）
 * @param max_vals  out_vals 能容纳的寄存器个数，防止写越界
 * @param out_exc   输出：异常码（仅当返回 MB_ERR_EXCEPTION 时有效）
 * @return MB_OK 正常；MB_ERR_EXCEPTION 异常帧；
 *         其余 MB_ERR_CRC / LEN / ADDR / FUNC / PARAM 对应各自的错误
 */
mb_status_t mb_parse_read_resp(const uint8_t *frame, uint16_t len,
                               uint8_t addr, uint8_t func,
                               uint16_t *out_vals, uint16_t max_vals,
                               uint8_t *out_exc);

/* ===================== CRC-16/MODBUS ===================== */
/**
 * @brief 计算 CRC-16/MODBUS（多项式 0xA001 反射，初值 0xFFFF，无最终异或）。
 * @param buf 待计算数据首地址（len=0 时允许为任意值）
 * @param len 待计算字节数（不包含 CRC 本身）
 * @return 16 位 CRC；发送和接收时都是低字节在前
 */
uint16_t mb_crc16(const uint8_t *buf, uint16_t len);

/* ===================== 组帧：读寄存器（0x03/0x04） ===================== */
/**
 * @brief 组装 8 字节读寄存器请求帧。
 * @param addr      从机地址（1~247）
 * @param func      功能码：MB_FC_READ_HOLDING(0x03) 或 MB_FC_READ_INPUT(0x04)
 * @param start_reg 起始寄存器地址（16 位，大端写入帧）
 * @param count     读取寄存器个数（1~125）
 * @param out       输出缓冲区，至少 8 字节；不能为 NULL
 * @return 成功返回 8（帧长）；入参非法返回 -1
 */
int mb_build_read_req(uint8_t addr, uint8_t func,
                      uint16_t start_reg, uint16_t count,
                      uint8_t *out);

/* ===================== 组帧：写单个寄存器（0x06） ===================== */
/**
 * @brief 组装 8 字节写单个保持寄存器请求帧。
 * @param addr  从机地址（1~247）
 * @param reg   目标寄存器地址（16 位，大端写入帧）
 * @param value 要写入的 16 位值（大端写入帧）
 * @param out   输出缓冲区，至少 8 字节；不能为 NULL
 * @return 成功返回 8（帧长）；入参非法返回 -1
 */
int mb_build_write_single(uint8_t addr, uint16_t reg,
                          uint16_t value, uint8_t *out);

#endif /* MODBUS_HOST_H */
