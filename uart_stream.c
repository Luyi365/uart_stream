/**
 * @file uart_stream.c
 * @author Luyi365 (luyi.hub@foxmail.com)
 * @brief 基于 UART的用户数据流缓冲处理
 * @date 2026-10-15
 * 
 * @copyright Copyright (c) 2026
 */

#include "uart_stream.h"
#include "uart_stream_packet.h"
#include "stdio.h"
#include "string.h"
#include "assert.h"

// 心跳时间换算为 tick，0 表示不启用
#define UART_STREAM_HEARTBEAT_TICKS ((UART_STREAM_HEARTBEAT_MS) == 0 ? portMAX_DELAY : pdMS_TO_TICKS(UART_STREAM_HEARTBEAT_MS))

typedef struct
{
    uint16_t len;
    uint8_t *data;
} UartStreamData_t;

typedef struct _MsgHandleNode_t
{
    uint32_t cmd_head;
    UartStreamMsgCallback callback;
    struct _MsgHandleNode_t *next;
} MsgHandleNode_t;

typedef struct _UartStreamAckNode_t
{
    uint32_t ack_cmd;
    SemaphoreHandle_t ack_sem;
    cJSON *data;
    struct _UartStreamAckNode_t *next;
    struct _UartStreamAckNode_t *prev;
} UartStreamAckNode_t;

// 发送
static QueueHandle_t UartStreamSendQueue = NULL;
static SemaphoreHandle_t UartStreamSendMutex = NULL;

// 接收：接收完成信号量与本次接收长度，由 UartStreamRxNotifyFromISR 写入
static QueueHandle_t UartStreamRecvQueue = NULL;
static SemaphoreHandle_t UartStreamRxSem = NULL;
static volatile uint16_t UartStreamRxLen = 0;

// 消息分发与应答
static MsgHandleNode_t *MsgHandleList = NULL;
static UartStreamAckNode_t *UartStreamAckList = NULL;

static void UartStreamMsgHandle(uint8_t *data);

/* =============================== 初始化 =============================== */

/** @brief 初始化 UART 数据流，详见 uart_stream.h */
void UartStreamInit(void)
{
    UartStreamSendQueue = xQueueCreate(UART_STREAM_SEND_QUEUE_LEN, sizeof(UartStreamData_t));
    UartStreamRecvQueue = xQueueCreate(UART_STREAM_RECV_QUEUE_LEN, sizeof(UartStreamData_t));
    UartStreamRxSem = xSemaphoreCreateBinary();

    if(pdTRUE != xTaskCreate(UartStreamSendTask, (const char *)"UartStreamSendTask", (uint16_t)1024, (void *)NULL, UART_STREAM_TASK_PRIORITY, NULL))
    {
        printf("UartStreamSendTask create fail\r\n");
        return;
    }
    if(pdTRUE != xTaskCreate(UartStreamRecvTask, (const char *)"UartStreamRecvTask", (uint16_t)2048, (void *)NULL, UART_STREAM_TASK_PRIORITY + 1, NULL))
    {
        printf("UartStreamRecvTask create fail\r\n");
        return;
    }
    UartStreamSendMutex = xSemaphoreCreateMutex();
    if(UartStreamSendMutex == NULL)
    {
        printf("UartStreamSendMutex create fail\r\n");
        return;
    }
    if(pdTRUE != xTaskCreate(recv_cb, "recv_cb", (uint16_t)1024, NULL, UART_STREAM_TASK_PRIORITY + 2, NULL))
    {
        printf("recv_cb create fail\r\n");
        return;
    }
}

/* ================================ 发送 ================================ */

/**
 * @brief 发送任务：从发送队列取出数据交给 UART 发送，空闲超时则发送心跳
 * @param arg 未使用
 */
static void UartStreamSendTask(void *arg)
{
    UartStreamData_t UartStreamSendData;

    while(1)
    {
        if(xQueueReceive(UartStreamSendQueue, &UartStreamSendData, UART_STREAM_HEARTBEAT_TICKS) == pdPASS)
        {
            assert(UartStreamSendData.data != NULL);
            uart_driver_send(UartStreamSendData.data, UartStreamSendData.len);	//让UART发送数据
            vPortFree(UartStreamSendData.data);
            vTaskDelay(pdMS_TO_TICKS(UART_STREAM_PKG_INTERVAL_MS));   //增加发送延迟，防止发送过快导致数据丢失
        }
        else
        {
            if(UART_STREAM_HEARTBEAT_MS != 0)
            {
                // 发送心跳包
                UartStreamSendString("{\"cmd\":805306373}");
            }
        }
    }
}

/** @brief 发送原始数据，详见 uart_stream.h */
void UartStreamSend(uint8_t *data, uint16_t len)
{
    UartStreamData_t UartStreamSendData;
    UartStreamSendData.len = len;
    UartStreamSendData.data = (uint8_t *)pvPortMalloc(len);
    assert(UartStreamSendData.data != NULL);
    memcpy(UartStreamSendData.data, data, len);
    if(pdTRUE != xQueueSend(UartStreamSendQueue, &UartStreamSendData, pdMS_TO_TICKS(100)))
    {
        vPortFree(UartStreamSendData.data);
        printf("UartStreamSendQueue full\r\n");
    }
}

/** @brief 组帧后发送数据，详见 uart_stream.h */
void UartStreamSendCMD(uint8_t *data, uint16_t len)
{
    xSemaphoreTake(UartStreamSendMutex, portMAX_DELAY);
    uint16_t datlen = len;
    char *dat = UartStreamPacketEncode((char *)data, &datlen);
    if(dat != NULL)
        UartStreamSend((uint8_t *)dat, datlen);
    else
        printf("packet encode fail, len %d\r\n", len);   // 数据为空或超过 UART_STREAM_PACKET_BUF_MAX
    xSemaphoreGive(UartStreamSendMutex);
}

/** @brief 组帧后发送字符串，详见 uart_stream.h */
void UartStreamSendString(char *str)
{
    UartStreamSendCMD((uint8_t *)str, strlen(str));
}

/* ================================ 接收 ================================ */

/** @brief 接收中断通知，详见 uart_stream.h */
void UartStreamRxNotifyFromISR(uint16_t len)
{
    BaseType_t woken = pdFALSE;
    if(UartStreamRxSem == NULL || len == 0)
        return;     // 未初始化或空数据
    UartStreamRxLen = len;
    xSemaphoreGiveFromISR(UartStreamRxSem, &woken);
    portYIELD_FROM_ISR(woken);
}

/**
 * @brief DMA 接收任务：收到中断通知后读出数据并放入接收队列
 * @param arg 未使用
 */
static void recv_cb(void *arg)
{
    while(1)
    {
        if(xSemaphoreTake(UartStreamRxSem, portMAX_DELAY) == pdTRUE) {
            UartStreamData_t UartStreamRecvData;
            UartStreamRecvData.len = UartStreamRxLen;
            UartStreamRecvData.data = (uint8_t *)pvPortMalloc(UartStreamRecvData.len);
            if(UartStreamRecvData.data == NULL)
            {
                printf("recv malloc fail, len %d\r\n", UartStreamRecvData.len);
                continue;
            }
            uart_driver_recv(UartStreamRecvData.data, UartStreamRecvData.len);
            // 判断队列是否已满
            if(uxQueueSpacesAvailable(UartStreamRecvQueue) == 0)
            {
                printf("UartStreamRecvQueue full\r\n");
                vPortFree(UartStreamRecvData.data);
            }
            else
            {
                xQueueSend(UartStreamRecvQueue, &UartStreamRecvData, portMAX_DELAY);
            }
        }
    }
}

/**
 * @brief 接收解析任务：从接收队列取出一帧，按帧头分流处理
 * @param arg 未使用
 */
static void UartStreamRecvTask(void *arg)
{
    UartStreamData_t UartStreamRecvData;

    while(1)
    {
        if(xQueueReceive(UartStreamRecvQueue, &UartStreamRecvData, UART_STREAM_HEARTBEAT_TICKS) == pdPASS)
        {
            assert(UartStreamRecvData.data != NULL);
            assert(UartStreamRecvData.len >= 2);
            if(UartStreamRecvData.data[0] == (uint8_t)((UART_STREAM_PKG_HEAD_OTA >> 8) & 0xFF) &&
               UartStreamRecvData.data[1] == (uint8_t)(UART_STREAM_PKG_HEAD_OTA & 0xFF))
            {
                //OTA
            }
            else if(UartStreamRecvData.data[0] == (uint8_t)UART_STREAM_PKG_HEAD_CMD)
            {
                //消息解析
                uint16_t msg_len = UartStreamRecvData.len;
                uint8_t *msg = (uint8_t *)UartStreamPacketDecode((char *)UartStreamRecvData.data, &msg_len);
                if(msg != NULL)
                {
                    UartStreamMsgHandle(msg);
                }
                else
                {
                    printf("packet decode fail\r\n");   // 拆包或 CRC 校验失败
                }
            }
            else if(UartStreamRecvData.data[0] == (uint8_t)UART_STREAM_PKG_HEAD_FACTORY)
            {
                //工厂/产测
            }
            else if(UartStreamRecvData.data[0] == (uint8_t)UART_STREAM_PKG_HEAD_HEARTBEAT)
            {
                //心跳：更新对端在线时间
            }
            else
            {
                //无效数据
            }
            vPortFree(UartStreamRecvData.data);
        }
        else
        {
            if(UART_STREAM_HEARTBEAT_MS != 0)
            {
                //  TODO 超时处理
                printf("recv timeout\r\n");
            }
        }
    }
}

/* ============================ 消息分发与应答 ============================ */

/**
 * @brief 在应答链表头部添加一个等待节点
 * @param ack_cmd 等待的应答命令字
 * @param ack_sem 收到应答时释放的信号量
 * @return 新建的节点
 */
static UartStreamAckNode_t *UartStreamAckListAddNode(uint32_t ack_cmd, SemaphoreHandle_t ack_sem)
{
    UartStreamAckNode_t *node = (UartStreamAckNode_t *)pvPortMalloc(sizeof(UartStreamAckNode_t));
    assert(node != NULL);
    node->ack_cmd = ack_cmd;
    node->ack_sem = ack_sem;
    node->data = NULL;
    node->next = UartStreamAckList;
    if(UartStreamAckList != NULL)
        UartStreamAckList->prev = node;
    node->prev = NULL;
    UartStreamAckList = node;
    return node;
}

/**
 * @brief 按应答命令字查找等待节点
 * @param ack_cmd 应答命令字
 * @return 找到的节点，未找到返回 NULL
 */
static UartStreamAckNode_t *UartStreamAckListQueryCmd(uint32_t ack_cmd)
{
    UartStreamAckNode_t *node = UartStreamAckList;
    while(node != NULL)
    {
        if(node->ack_cmd == ack_cmd)
        {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

/**
 * @brief 从应答链表中删除并释放节点
 * @param node 待删除的节点
 */
static void UartStreamAckListDeleteNode(UartStreamAckNode_t *node)
{
    if(node == UartStreamAckList)
    {
        UartStreamAckList = node->next;
        if(UartStreamAckList != NULL)
            UartStreamAckList->prev = NULL;
    }
    else
    {
        node->prev->next = node->next;
        // 删除非头节点时同步更新后继节点的prev指针
        if(node->next != NULL)
            node->next->prev = node->prev;
    }
    vPortFree(node);
}

/**
 * @brief 解析 JSON 消息：先唤醒等待该应答的任务，再按命令字分组调用回调
 * @param data 以 '\0' 结尾的 JSON 字符串
 */
static void UartStreamMsgHandle(uint8_t *data)
{
    cJSON *parse = cJSON_Parse((char *)data);
    if(parse != NULL)
    {
        cJSON *cmd = cJSON_GetObjectItem(parse, "cmd");
        if(cmd != NULL)
        {
            // 处理应答请求
            {
                UartStreamAckNode_t *node = NULL;
                node = UartStreamAckListQueryCmd(cmd->valueint);
                if(node != NULL)
                {
                    node->data = cJSON_Duplicate(parse, 1);
                    printf("recv ack cmd: 0x%x\r\n", cmd->valueint);
                    xSemaphoreGive(node->ack_sem);
                }
            }
            // 处理消息
            {
                MsgHandleNode_t *node = MsgHandleList;
                while(node != NULL)
                {
                    if((cmd->valueint & 0xFFFF0000) == node->cmd_head)
                    {
                        if(node->callback != NULL)
                        {
                            node->callback(parse);
                            goto handle_end;
                        }
                        else
                        {
                            printf("message 0x%x callback is NULL\r\n", cmd->valueint);
                        }
                    }
                    node = node->next;
                }
                printf("message 0x%x not handle\r\n", cmd->valueint);
            }
        }
        else
        {
            printf("message cmd is NULL\r\n");
        }
    handle_end:
        cJSON_Delete(parse);
    }
}

/** @brief 注册消息回调，详见 uart_stream.h */
void UartStreamRegisterMsgHandle(uint32_t cmd_head, UartStreamMsgCallback callback)
{
    MsgHandleNode_t *node = (MsgHandleNode_t *)pvPortMalloc(sizeof(MsgHandleNode_t));
    assert(node != NULL);
    node->cmd_head = cmd_head;
    node->callback = callback;
    node->next = MsgHandleList;
    MsgHandleList = node;
}

/** @brief 发送并等待应答，详见 uart_stream.h */
BaseType_t UartStreamSendWaitAck(char *str, uint32_t cmd, TickType_t delay, cJSON **ack)
{
    SemaphoreHandle_t ack_sem = xSemaphoreCreateBinary();
    assert(ack_sem != NULL);
    // 先登记等待再发送，避免应答在开始等待前到达而丢失
    UartStreamAckNode_t *node = UartStreamAckListAddNode(UART_STREAM_ACK_CMD(cmd), ack_sem);
    printf("wait ack 0x%lx\r\n", (unsigned long)UART_STREAM_ACK_CMD(cmd));
    UartStreamSendString(str);
    BaseType_t ret = xSemaphoreTake(ack_sem, delay);
    if(ack != NULL)
    {
        *ack = node->data;
    }
    else
    {
        // 释放内存
        if(node->data != NULL)
            cJSON_Delete(node->data);
    }
    vSemaphoreDelete(ack_sem);
    UartStreamAckListDeleteNode(node);
    return ret;
}
