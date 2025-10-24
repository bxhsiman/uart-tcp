#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"

/* 数据包和帧处理 */
typedef struct {
    uint8_t header[2];      // 0x0A 0x00
    uint8_t sequence;       // 0-7
    uint8_t reserved;       // 0x00
    uint8_t data[40];       // 40字节数据
} lidar_packet_t;

typedef struct {
    lidar_packet_t packets[LIDAR_FRAME_PACKETS];
    bool valid;
    uint32_t timestamp;
} lidar_frame_t;

/* 全局变量声明 */
extern QueueHandle_t g_frame_queue;        // 帧队列（生产者-消费者）
extern QueueHandle_t g_uart_event_queue;   // UART事件队列
extern SemaphoreHandle_t g_sock_mutex;
extern int g_sock;

/* 函数声明 */
bool validate_lidar_packet(const uint8_t* data, int len);
bool validate_frame(const lidar_frame_t* frame);
bool send_frame(const lidar_frame_t* frame);  // 发送单帧
void init_data_processing(void);
void uart_event_task(void *arg);              // UART事件处理任务（中断驱动）
void tcp_send_task(void *arg);                // 消费者：TCP发送