/**
 * @file uart_stream_packet.c
 * @author Luyi365 (luyi.hub@foxmail.com)
 * @brief 数据组帧和解帧处理
 * @date 2026-10-15
 * 
 * @copyright Copyright (c) 2026
 */

#include "uart_stream_packet.h"
#include "uart_stream.h"
#include "string.h"

#define KEY 0x5b    // 异或密钥

#define MESSAGE_PACK_END '#'    // 帧尾，帧头即 UART_STREAM_PKG_HEAD_CMD

// 组帧增加的字节数：帧头帧尾 2 字节 + CRC 1 字节
#define PACKET_OVERHEAD (__PACK_EN * 2 + __CRC_EN)

/* ================================ 公共 ================================ */

#if __CRC_EN
/**
 * @brief 计算 CRC8（多项式 0x31，反射输入输出，即 CRC-8/MAXIM）
 * @param ptr 数据
 * @param len 数据长度（字节）
 * @return CRC8 校验值
 */
static uint8_t calc_crc8(void *ptr, size_t len)
{
    uint8_t *p;
    p = (uint8_t *)ptr;
    uint8_t crc;
    uint8_t i;
    crc = 0;
    while(len--)
    {
        crc ^= *p++;
        for(i = 0; i < 8; i++)
        {
            if(crc & 0x01)
                crc = (crc >> 1) ^ 0x8C;
            else
                crc >>= 1;
        }
    }
    return crc;
}
#endif

#if __CODING_EN
/**
 * @brief 异或编解码（编码与解码相同）
 * @param buf 输出缓冲，由调用方提供，收发各用一块避免互相覆盖
 * @param str 输入数据
 * @param len 数据长度（字节），不变
 * @return buf
 */
static char *coding(char *buf, char *str, uint16_t *len)
{
    memset(buf, 0, UART_STREAM_PACKET_BUF_MAX);

    for(int i = 0; i < *len; i++)
    {
        buf[i] = str[i] ^ KEY;
    }
    return buf;
}
#endif

/* ============================== 发送：组帧 ============================== */

#if __CRC_EN
/**
 * @brief 在数据末尾追加 1 字节 CRC8
 * @param str 原始数据
 * @param len 输入为原始长度，输出加 1
 * @return 追加 CRC 后的数据（内部静态缓冲）
 */
static char *add_CRC(char *str, uint16_t *len)
{
    static char ret[UART_STREAM_PACKET_BUF_MAX];
    memset(ret, 0, UART_STREAM_PACKET_BUF_MAX);

    memcpy(ret, str, *len);
    ret[*len] = calc_crc8(ret, *len);
    *len += 1;
    return ret;
}
#endif

#if __PACK_EN
/**
 * @brief 添加帧头 UART_STREAM_PKG_HEAD_CMD 和帧尾 MESSAGE_PACK_END
 * @param str 数据
 * @param len 输入为数据长度，输出加 2
 * @return 组帧后的数据（内部静态缓冲）
 */
static char *pack(char *str, uint16_t *len)
{
    static char ret[UART_STREAM_PACKET_BUF_MAX];
    memset(ret, 0, UART_STREAM_PACKET_BUF_MAX);

    ret[0] = UART_STREAM_PKG_HEAD_CMD;
    memcpy(ret + 1, str, *len);
    ret[*len + 1] = MESSAGE_PACK_END;
    *len += 2;

    return ret;
}
#endif

/** @brief 组帧，详见 uart_stream_packet.h */
char *UartStreamPacketEncode(char *str, uint16_t *len)
{
    // 组帧后长度需小于缓冲区，保证结尾留有 '\0'
    if(str == NULL || *len == 0 || *len + PACKET_OVERHEAD >= UART_STREAM_PACKET_BUF_MAX)
    {
        return NULL;
    }
    char *ret = str;
#if __CRC_EN
    ret = add_CRC(ret, len);
#endif
#if __CODING_EN
    static char buf[UART_STREAM_PACKET_BUF_MAX];
    ret = coding(buf, ret, len);
#endif
#if __PACK_EN
    ret = pack(ret, len);
#endif
    return ret;
}

/* ============================== 接收：解帧 ============================== */

#if __PACK_EN
/**
 * @brief 校验并去除帧头帧尾
 * @param str 一帧数据
 * @param len 输入为帧长度，输出减 2
 * @return 去除帧头帧尾后的数据（内部静态缓冲），帧头或帧尾不匹配返回 NULL
 */
static char *unpack(char *str, uint16_t *len)
{
    static char ret[UART_STREAM_PACKET_BUF_MAX];
    memset(ret, 0, UART_STREAM_PACKET_BUF_MAX);

    if((str[0] == UART_STREAM_PKG_HEAD_CMD) && (str[*len - 1] == MESSAGE_PACK_END))
    {
        memcpy(ret, str + 1, *len - 2);
        *len -= 2;
        return ret;
    }
    else
    {
        return NULL;
    }
}
#endif

#if __CRC_EN
/**
 * @brief 校验并去除末尾 1 字节 CRC8（_DEBUG 下不校验）
 * @param str 带 CRC 的数据
 * @param len 输入为带 CRC 长度，输出减 1
 * @return 去除 CRC 后的数据（内部静态缓冲），校验失败返回 NULL
 */
static char *remove_CRC(char *str, uint16_t *len)
{
    static char ret[UART_STREAM_PACKET_BUF_MAX];
    memset(ret, 0, UART_STREAM_PACKET_BUF_MAX);

#if !_DEBUG  //调试模式下不检查CRC
    if(calc_crc8(str, *len - 1) != (uint8_t)str[*len - 1])
    {
        return NULL;
    }
#endif
    memcpy(ret, str, *len - 1);
    *len -= 1;
    return ret;
}
#endif

/** @brief 解帧，详见 uart_stream_packet.h */
char *UartStreamPacketDecode(char *str, uint16_t *len)
{
    // 输入不得小于组帧开销，且需小于缓冲区，保证结尾留有 '\0'
    if(str == NULL || *len < PACKET_OVERHEAD || *len >= UART_STREAM_PACKET_BUF_MAX)
    {
        return NULL;
    }
    char *ret = str;
#if __PACK_EN
    ret = unpack(ret, len);
#endif
#if __CODING_EN
    if(ret != NULL)
    {
        static char buf[UART_STREAM_PACKET_BUF_MAX];
        ret = coding(buf, ret, len);
    }
#endif
#if __CRC_EN
    if(ret != NULL)
    {
        ret = remove_CRC(ret, len);
    }
#endif
    return ret;
}
