#include "modbus_host.h"

/* ================= CRC-16/MODBUS 查表法 ================= */
static uint16_t s_crc_tab[256];
static int      s_crc_ready = 0;

static void crc16_build_table(void)
{
    for (int i = 0; i < 256; i++)
    {
        uint16_t c = (uint16_t)i; /* 从字节值起跑，不是 0xFFFF */
        for (int j = 0; j < 8; j++)
        {
            c = (c & 1u) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
        }
        s_crc_tab[i] = c;
    }
    s_crc_ready = 1;
}

/**
 * @brief 计算 CRC-16/MODBUS。
 * @param buf 待计算数据首地址
 * @param len 待计算字节数（不含 CRC 本身）
 * @return 16 位 CRC（发送/接收时都是低字节在前）
 */
uint16_t mb_crc16(const uint8_t *buf, uint16_t len)
{
    if (!s_crc_ready)
    {
        crc16_build_table();
    }

    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++)
    {
        uint8_t idx = (uint8_t)((crc ^ buf[i]) & 0xFF);
        crc = (uint16_t)((crc >> 8) ^ s_crc_tab[idx]);
    }
    return crc;
}

/* ================= 请求帧组装 ================= */

/**
 * @brief 组装读寄存器请求帧（0x03/0x04）。
 * @param addr      从机地址（1~247）
 * @param func      功能码（0x03 或 0x04）
 * @param start_reg 起始寄存器地址
 * @param count     读取寄存器个数（1~125）
 * @param out       输出缓冲区（至少 8 字节）
 * @return 8=成功；-1=入参非法
 */
int mb_build_read_req(uint8_t addr,        /* 从机地址 1~247 */
                      uint8_t func,        /* 0x03=保持寄存器, 0x04=输入寄存器 */
                      uint16_t start_reg,  /* 起始寄存器地址 */
                      uint16_t count,      /* 读取数量 1~125 */
                      uint8_t *out)        /* 输出：8 字节请求帧 */
{
    if (out == 0)                                               return -1;
    if (addr < MB_ADDR_MIN || addr > MB_ADDR_MAX)               return -1;
    if (func != MB_FC_READ_HOLDING && func != MB_FC_READ_INPUT) return -1;
    if (count < 1u || count > MB_READ_MAX)                      return -1;

    out[0] = addr;                                  /* 从机地址 */
    out[1] = func;                                  /* 功能码 */
    out[2] = (uint8_t)((start_reg >> 8) & 0xFF);    /* 寄存器地址高字节（大端） */
    out[3] = (uint8_t)( start_reg       & 0xFF);    /* 寄存器地址低字节 */
    out[4] = (uint8_t)((count >> 8)     & 0xFF);    /* 数量高字节（大端） */
    out[5] = (uint8_t)( count           & 0xFF);    /* 数量低字节 */

    uint16_t crc = mb_crc16(out, 6);                /* CRC 只算前 6 字节 */
    out[6] = (uint8_t)( crc       & 0xFF);          /* CRC 低字节在前（小端） */
    out[7] = (uint8_t)((crc >> 8) & 0xFF);          /* CRC 高字节在后 */
    return 8;
}

/**
 * @brief 组装写单个保持寄存器请求帧（0x06）。
 * @param addr  从机地址（1~247）
 * @param reg   目标寄存器地址
 * @param value 要写入的 16 位值
 * @param out   输出缓冲区（至少 8 字节）
 * @return 8=成功；-1=入参非法
 */
int mb_build_write_single(uint8_t addr,      /* 从机地址 1~247 */
                          uint16_t reg,      /* 目标寄存器地址 */
                          uint16_t value,    /* 要写入的值 */
                          uint8_t *out)      /* 输出：8 字节请求帧 */
{
    if (out == 0)                                  return -1;
    if (addr < MB_ADDR_MIN || addr > MB_ADDR_MAX)  return -1;

    out[0] = addr;                                 /* 从机地址 */
    out[1] = MB_FC_WRITE_SINGLE;                   /* 功能码 0x06 */
    out[2] = (uint8_t)((reg   >> 8) & 0xFF);       /* 寄存器地址高字节（大端） */
    out[3] = (uint8_t)( reg         & 0xFF);       /* 寄存器地址低字节 */
    out[4] = (uint8_t)((value >> 8) & 0xFF);       /* 写入值高字节（大端） */
    out[5] = (uint8_t)( value       & 0xFF);       /* 写入值低字节 */

    uint16_t crc = mb_crc16(out, 6);
    out[6] = (uint8_t)( crc       & 0xFF);         /* CRC 低字节在前（小端） */
    out[7] = (uint8_t)((crc >> 8) & 0xFF);         /* CRC 高字节在后 */
    return 8;
}

/* ================= 响应帧解析 ================= */

/**
 * @brief 解析 0x03/0x04 的响应帧（正常帧和异常帧都支持）。
 * @param frame    收到的完整帧
 * @param len      帧总字节数（含 CRC）
 * @param addr     期望从机地址
 * @param func     期望功能码（0x03 或 0x04）
 * @param out_vals 输出：寄存器值数组
 * @param max_vals out_vals 能容纳的寄存器个数
 * @param out_exc  输出：异常码（MB_ERR_EXCEPTION 时有效）
 * @return MB_OK / MB_ERR_* 见 mb_status_t
 */
mb_status_t mb_parse_read_resp(const uint8_t *frame,  /* 完整响应帧 */
                               uint16_t len,          /* 帧总长（含 CRC） */
                               uint8_t addr,          /* 期望从机地址 */
                               uint8_t func,          /* 期望功能码 0x03/0x04 */
                               uint16_t *out_vals,    /* 输出：寄存器值数组 */
                               uint16_t max_vals,     /* out_vals 容量 */
                               uint8_t *out_exc)      /* 输出：异常码 */
{
    if (frame == 0 || out_vals == 0 || out_exc == 0) return MB_ERR_PARAM;
    if (func != MB_FC_READ_HOLDING && func != MB_FC_READ_INPUT) return MB_ERR_PARAM;

    /* ① 长度下限：最短的异常帧就是 5 字节 */
    if (len < 5u) return MB_ERR_LEN;

    /* ② 从机地址（可能是总线上别的从站的回应） */
    if (frame[0] != addr) return MB_ERR_ADDR;

    /* ③ 异常帧：功能码最高位置 1（0x83 / 0x84），固定 5 字节 */
    if (frame[1] == (uint8_t)(func | 0x80u))
    {
        if (len != 5u) return MB_ERR_LEN;

        /* 异常帧也要验 CRC：地址 + 功能码 + 异常码，共 3 字节 */
        uint16_t crc_calc = mb_crc16(frame, 3u);
        if (frame[3] != (uint8_t)( crc_calc       & 0xFF) ||
            frame[4] != (uint8_t)((crc_calc >> 8) & 0xFF))
            return MB_ERR_CRC;

        *out_exc = frame[2];
        return MB_ERR_EXCEPTION;
    }

    /* 正常帧：功能码必须与期望一致 */
    if (frame[1] != func) return MB_ERR_FUNC;

    /* ④ 字节数自洽：字节数 = 2N，且与帧长相符 */
    uint8_t byte_cnt = frame[2];
    if (byte_cnt == 0u || (byte_cnt & 1u)) return MB_ERR_LEN;  /* 必须是正偶数 */
    if (len != (uint16_t)(3u + byte_cnt + 2u)) return MB_ERR_LEN;

    /* ⑤ CRC：对「除末尾 2 字节外」的全部重算、比对 */
    uint16_t crc_calc = mb_crc16(frame, (uint16_t)(len - 2u));
    if (frame[len - 2u] != (uint8_t)( crc_calc       & 0xFF) ||
        frame[len - 1u] != (uint8_t)((crc_calc >> 8) & 0xFF))
        return MB_ERR_CRC;

    /* ⑥ 大端拼值：每 2 字节一个寄存器 */
    uint16_t n = (uint16_t)(byte_cnt / 2u);
    if (n > max_vals) return MB_ERR_PARAM;               /* 防止写越界 out_vals */
    for (uint16_t i = 0; i < n; i++)
        out_vals[i] = (uint16_t)(((uint16_t)frame[3u + i * 2u] << 8) |
                                   (uint16_t)frame[4u + i * 2u]);
    return MB_OK;
}

/* ================= 双超时事务 ================= */
#define MB_UART_BITS_PER_CHAR  10u       /* 8N1：1 起始 + 8 数据 + 0 校验 + 1 停止 */
#define MB_BAUD                9600u
#define MB_CHAR_US  ((1000000u * MB_UART_BITS_PER_CHAR) / MB_BAUD)   /* ≈1041us */
#define MB_T1_5_MS  ((MB_CHAR_US * 15u / 10u + 999u) / 1000u)        /* 帧内最大间隔 ≈2ms（预留） */
#define MB_T3_5_MS  ((MB_CHAR_US * 35u / 10u + 999u) / 1000u)        /* 帧结束静默 ≈4ms */
#define MB_RESP_TIMEOUT_MS  (200u)       /* 整帧超时；读 125 个寄存器时需放大到 >271ms */

/**
 * @brief 发送请求帧并接收应答（字节间静默 + 整帧两层超时）。
 * @param port     移植层函数表
 * @param req      请求帧
 * @param req_len  请求帧长度
 * @param resp     应答帧缓冲区
 * @param resp_cap 应答缓冲区容量（字节）
 * @param resp_len 输出：实际收到的字节数（失败时置 0）
 * @return MB_OK / MB_ERR_TIMEOUT / MB_ERR_LEN / MB_ERR_IO / MB_ERR_PARAM
 */
mb_status_t mb_transact(const mb_port_t *port,   /* 移植层：write/read_byte/tick_ms */
                        const uint8_t *req,      /* 请求帧首地址 */
                        uint16_t req_len,        /* 请求帧长度（0x03/0x04 为 8） */
                        uint8_t *resp,           /* 应答帧接收缓冲区 */
                        uint16_t resp_cap,       /* 应答缓冲区容量，防越界 */
                        uint16_t *resp_len)      /* 输出：实际收到的字节数 */
{
    if (!port || !port->write || !port->read_byte || !port->tick_ms) return MB_ERR_PARAM;
    if (!req || !resp || !resp_len)                                  return MB_ERR_PARAM;

    *resp_len = 0;                                  /* 失败时调用方也能安全读取 */

    if (port->write(req, req_len, port->ctx) != 0)  return MB_ERR_IO;

    uint32_t start = port->tick_ms();
    uint16_t n = 0;
    for (;;)
    {
        uint32_t elapsed = port->tick_ms() - start;
        if (elapsed > MB_RESP_TIMEOUT_MS)           /* 整帧超时：从机完全没反应 */
            return MB_ERR_TIMEOUT;

        uint8_t b;
        /* 首字节：最多等到整帧超时；后续字节：等 t3.5 静默，静默到了就认为帧结束 */
        uint32_t wait = (n == 0u) ? (MB_RESP_TIMEOUT_MS - elapsed) : MB_T3_5_MS;
        if (wait == 0u) wait = 1u;

        if (port->read_byte(&b, wait, port->ctx) != 0)
        {
            if (n == 0u) return MB_ERR_TIMEOUT;     /* 一个字节都没来 */
            break;                                   /* 收到若干字节后安静了 → 帧结束 */
        }
        if (n >= resp_cap) return MB_ERR_LEN;        /* 缓冲装不下，防越界 */
        resp[n++] = b;
    }
    *resp_len = n;
    return MB_OK;
}
