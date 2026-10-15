# uart_stream

基于 FreeRTOS 的串口数据流处理库，用于**板间通信**。  
消息以 JSON 格式传输，负责收发缓冲、组帧解帧、CRC 校验、按命令字分发和请求-应答等待。

## 特性

- 收发各有独立任务和队列，发送接口线程安全，可在多个任务中调用
- 接收配合 **DMA + 空闲中断**：驱动在空闲中断中通知库，由库任务取出整帧数据
- 帧头分流：OTA、命令消息、工厂/产测、心跳
- 组帧：帧头 + 数据 + CRC8 + 帧尾，可选异或编码
- 按命令字分组回调，支持发送后等待应答
- 发送空闲时自动发送心跳
- 依赖 FreeRTOS 和 cJSON 库

## 数据格式

### 帧结构

```
| '&' | JSON 数据 | CRC8 | '#' |
```

- CRC8 为 CRC-8/MAXIM，覆盖 JSON 数据
- `uart_stream_packet.h` 中 `__PACK_EN` / `__CRC_EN` / `__CODING_EN` 分别开关帧头帧尾、CRC、异或编码

### 命令字

消息必须包含 `"cmd"` 字段（32 位整数）：

```
| bit31-30 | bit29-16 | bit15-0 |
|  保留    |   分组   |  命令号  |
```

- 回调按高 16 位分组分发
- 应答命令字 = 请求命令字 `| 0x40000000`，请求命令字不得使用 bit30、bit31

### 命令示例

```json
// 请求 0x00010001
{"cmd":65537,"value":1}

// 应答 0x40010001
{"cmd":1073807361,"result":0}

// 心跳 0x30000005
{"cmd":805306373}
```

## API

| 接口 | 作用 |
|---|---|
| `void UartStreamInit(void)` | 初始化，创建队列、信号量和任务 |
| `void UartStreamRxNotifyFromISR(uint16_t len)` | 在接收空闲中断中调用，通知收到一帧 |
| `void UartStreamSendString(char *str)` | 组帧后发送字符串 |
| `void UartStreamSendCMD(uint8_t *data, uint16_t len)` | 组帧后发送数据 |
| `void UartStreamSend(uint8_t *data, uint16_t len)` | 不组帧，直接发送数据 |
| `void UartStreamRegisterMsgHandle(uint32_t cmd_head, UartStreamMsgCallback cb)` | 注册分组回调 |
| `BaseType_t UartStreamSendWaitAck(char *str, uint32_t cmd, TickType_t delay, cJSON **ack)` | 发送请求并等待应答 |

### 示例

```c
#include "uart_stream.h"

// 分组 0x0001 的回调，json 由库释放
static void OnGroup0001(cJSON *json)
{
    cJSON *value = cJSON_GetObjectItem(json, "value");
}

void app_init(void)
{
    UartStreamInit();
    UartStreamRegisterMsgHandle(0x00010000, OnGroup0001);
}

void app_task(void *arg)
{
    cJSON *ack = NULL;

    // 发送 0x00010001，最多等待 500ms
    if(UartStreamSendWaitAck("{\"cmd\":65537,\"value\":1}", 0x00010001,
                             pdMS_TO_TICKS(500), &ack) == pdTRUE && ack != NULL)
    {
        cJSON_Delete(ack);
    }
}
```

## 配置宏

| 宏 | 默认值 | 说明 |
|---|---|---|
| `UART_STREAM_PKG_HEAD_OTA` | `0xA55A` | OTA 帧头（2 字节） |
| `UART_STREAM_PKG_HEAD_CMD` | `'&'` | 命令消息帧头 |
| `UART_STREAM_PKG_HEAD_FACTORY` | `'#'` | 工厂/产测帧头 |
| `UART_STREAM_PKG_HEAD_HEARTBEAT` | `'@'` | 心跳帧头 |
| `UART_STREAM_TASK_PRIORITY` | `configMAX_PRIORITIES - 3` | 发送任务优先级，接收解析 +1，DMA 接收 +2 |
| `UART_STREAM_SEND_QUEUE_LEN` | `5` | 发送队列长度 |
| `UART_STREAM_RECV_QUEUE_LEN` | `20` | 接收队列长度 |
| `UART_STREAM_PKG_INTERVAL_MS` | `10` | 每包发送间隔 |
| `UART_STREAM_HEARTBEAT_MS` | `1000` | 心跳及接收超时时间，0 为关闭 |
| `UART_STREAM_PACKET_BUF_MAX` | `512` | 单帧最大长度，不大于驱动收发缓冲 |

## 移植

用户需实现：

```c
// DMA 发送，返回前需把数据拷入驱动缓冲
void uart_driver_send(uint8_t *data, uint16_t len);

// 从 DMA 接收缓冲读出数据
void uart_driver_recv(uint8_t *data, uint16_t len);

// 空闲中断中通知库
void USARTx_IRQHandler(void)
{
    if(/* 空闲中断 */)
    {
        UartStreamRxNotifyFromISR(/* 本帧长度 */);
    }
}
```

此外：

- `printf` 重定向（库内日志）
- `cJSON_InitHooks` 指向 `pvPortMalloc` / `vPortFree`
- 按负载调整 `configTOTAL_HEAP_SIZE`
