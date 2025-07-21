#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
extern lidar_frame_t g_frame_buffer[FRAME_BUFFER_COUNT];
extern int g_current_frame_index;
extern int g_buffered_frames;
extern SemaphoreHandle_t g_frame_mutex;
extern SemaphoreHandle_t g_sock_mutex;
extern int g_sock;

/* 函数声明 */
bool validate_lidar_packet(const uint8_t* data, int len);
bool validate_frame(const lidar_frame_t* frame);
void send_buffered_frames(void);
void init_data_processing(void);
void uart_to_sock_task(void *arg);