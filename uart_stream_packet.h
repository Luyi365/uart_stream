/**
 * @file uart_stream_packet.h
 * @author Luyi365 (luyi.hub@foxmail.com)
 * @brief 数据组帧和解帧处理
 * @date 2026-10-15
 * 
 * @copyright Copyright (c) 2026
 */
 
#ifndef __UART_STREAM_PACKET_H__
#define __UART_STREAM_PACKET_H__

#include "stdint.h"

#define __PACK_EN 1
#define __CRC_EN 1
#define __CODING_EN 0

// 单帧最大长度（含帧头帧尾和 CRC），收发共用，不得大于 UART 驱动的收发缓冲区
#define UART_STREAM_PACKET_BUF_MAX 512

/**
 * @brief 组帧：依次添加 CRC、编码、帧头帧尾
 * @param str 原始数据
 * @param len 输入为原始长度，输出为组帧后长度
 * @return 组帧结果（内部静态缓冲，下次调用前有效），失败返回 NULL
 */
char *UartStreamPacketEncode(char *str, uint16_t *len);

/**
 * @brief 解帧：依次去除帧头帧尾、解码、校验并去除 CRC
 * @param str 接收到的一帧数据
 * @param len 输入为帧长度，输出为数据长度
 * @return 以 '\0' 结尾的数据（内部静态缓冲，下次调用前有效），失败返回 NULL
 */
char *UartStreamPacketDecode(char *str, uint16_t *len);

#endif
