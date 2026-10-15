/**
 * @file uart_stream.h
 * @author Luyi365 (luyi.hub@foxmail.com)
 * @brief 基于 UART的用户数据流缓冲处理
 * @date 2026-10-15
 * 
 * @copyright Copyright (c) 2026
 */

#ifndef __UART_STREAM_H__
#define __UART_STREAM_H__

#include "stdint.h"
#include "stddef.h"
#include "cJSON/cJSON.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

// OTA 帧头（2 字节）：高字节为第 1 个字节，低字节为第 2 个字节
#define UART_STREAM_PKG_HEAD_OTA (0xA55AU)

// 命令帧头（1 字节）
#define UART_STREAM_PKG_HEAD_CMD ('&')

// 工厂/产测帧头（1 字节）
#define UART_STREAM_PKG_HEAD_FACTORY ('#')

// 心跳帧头（1 字节）
#define UART_STREAM_PKG_HEAD_HEARTBEAT ('@')

// 任务基础优先级：发送任务 = 基础，接收解析任务 = 基础 + 1，DMA 接收任务 = 基础 + 2
// 需保证 基础 + 2 < configMAX_PRIORITIES
#define UART_STREAM_TASK_PRIORITY (configMAX_PRIORITIES - 3)

// 发送/接收队列长度，不能为 0
#define UART_STREAM_SEND_QUEUE_LEN (5)
#define UART_STREAM_RECV_QUEUE_LEN (20)

// 每包发送后的间隔时间（ms），防止发送过快导致对端丢数据
#define UART_STREAM_PKG_INTERVAL_MS (10)

// 心跳时间（ms），收发共用：发送空闲超过该时间发送心跳，接收超过该时间未收到数据视为超时
// 设为 0 表示不发送心跳、不检测接收超时
#define UART_STREAM_HEARTBEAT_MS (1000)


/* ---------------- 移植接口：由用户在 UART 驱动中实现 ---------------- */
/**
 * @brief 通过 UART 发送数据
 * @note  返回前必须把数据拷走，库会在返回后立即释放 data
 * @param data 待发送数据
 * @param len  数据长度（字节）
 */
void uart_driver_send(uint8_t *data, uint16_t len);

/**
 * @brief 从 DMA 接收缓冲读出数据
 * @param data 输出缓冲区
 * @param len  读取长度（字节）
 */
void uart_driver_recv(uint8_t *data, uint16_t len);
/* -------------------------------------------------------------------- */

// 应答标志位：应答命令字 = 请求命令字 | 应答标志
// 使用 bit30 而非 bit31，因为 cJSON 的 valueint 为有符号 int，超过 0x7FFFFFFF 会被截断
// 请求命令字不得使用 bit30、bit31
#define UART_STREAM_ACK_FLAG (0x40000000UL)
#define UART_STREAM_ACK_CMD(cmd) ((uint32_t)(cmd) | UART_STREAM_ACK_FLAG)

/**
 * @brief 消息回调函数类型
 * @param json 解析后的消息，回调返回后由库释放，不得保存或释放
 */
typedef void (*UartStreamMsgCallback)(cJSON *json);

/* =============================== 初始化 =============================== */

/**
 * @brief 初始化 UART 数据流：创建队列、信号量和收发任务
 */
void UartStreamInit(void);

/* ================================ 发送 ================================ */

/**
 * @brief 发送原始数据（不组帧），数据会被拷贝后放入发送队列
 * @param data 待发送数据
 * @param len  数据长度（字节）
 */
void UartStreamSend(uint8_t *data, uint16_t len);

/**
 * @brief 组帧后发送数据
 * @param data 待发送数据
 * @param len  数据长度（字节）
 */
void UartStreamSendCMD(uint8_t *data, uint16_t len);

/**
 * @brief 组帧后发送字符串
 * @param str 以 '\0' 结尾的字符串
 */
void UartStreamSendString(char *str);

/* ================================ 接收 ================================ */

/**
 * @brief 通知库收到一帧数据，由 UART 驱动在接收空闲中断中调用
 * @param len 本次接收到的数据长度（字节）
 */
void UartStreamRxNotifyFromISR(uint16_t len);

/* ============================ 消息分发与应答 ============================ */

/**
 * @brief 注册消息回调，命令字高 16 位等于 cmd_head 的消息会交给该回调
 * @param cmd_head 命令字分组（高 16 位，低 16 位为 0）
 * @param callback 消息回调
 */
void UartStreamRegisterMsgHandle(uint32_t cmd_head, UartStreamMsgCallback callback);

/**
 * @brief 发送请求并阻塞等待其应答
 * @note  内部先登记等待再发送，保证不会错过快速返回的应答
 * @note  不得在消息回调中调用：回调运行在接收任务中，阻塞会导致应答无法被处理
 * @param str   请求消息（JSON 字符串）
 * @param cmd   请求命令字，应答命令字为 UART_STREAM_ACK_CMD(cmd)
 * @param delay 最长等待时间（tick）
 * @param ack   输出应答消息，使用后需调用 cJSON_Delete 释放；超时输出 NULL；传 NULL 则由库释放
 * @return pdTRUE 收到应答，pdFALSE 超时
 */
BaseType_t UartStreamSendWaitAck(char *str, uint32_t cmd, TickType_t delay, cJSON **ack);

#endif
