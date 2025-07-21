#ifndef LIDAR_PACKET_H
#define LIDAR_PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* LiDAR数据包缓存结构 */
typedef struct {
    uint8_t packet[LIDAR_PACKET_SIZE];
    bool received;
    uint32_t timestamp;
} lidar_cached_packet_t;

/* LiDAR数据包统计结构 */
typedef struct {
    uint32_t total_bytes_received;
    uint32_t packets_detected;
    uint32_t packets_lost;
    uint32_t sequence_errors;
    uint8_t last_sequence;
    bool first_packet;
    uint32_t last_log_time;
    uint8_t pending_buffer[LIDAR_PACKET_SIZE];  // 跨边界缓冲区
    int pending_len;
    
    // 批量缓存相关
    lidar_cached_packet_t packet_cache[LIDAR_BATCH_SIZE];  // 缓存8个包
    uint32_t cache_received_mask;  // 位掩码标记哪些序号已收到
    uint32_t batches_sent;
    uint32_t incomplete_batches;
} lidar_stats_t;

/* LiDAR数据包批次队列项 */
typedef struct {
    uint8_t batch_data[LIDAR_BATCH_SIZE * LIDAR_PACKET_SIZE];  // 8个352字节包 = 2816字节
    uint32_t timestamp;
    uint8_t sequence_mask;  // 包含的序号掩码
} lidar_batch_item_t;

/* LiDAR包处理函数 */
void lidar_packet_init(void);
void lidar_reset_stats(void);
bool lidar_validate_packet(uint8_t *packet);
void lidar_analyze_data(uint8_t *data, int len);
void lidar_cache_packet(uint8_t *packet);  // 新增：缓存单个包
void lidar_check_and_send_batch(void);     // 新增：检查并发送完整批次
void lidar_start_tcp_sender_task(void);
QueueHandle_t lidar_get_tcp_queue(void);
lidar_stats_t* lidar_get_stats(void);

/* TCP发送回调函数类型 */
typedef void (*tcp_send_callback_t)(uint8_t *data, size_t len);
void lidar_set_tcp_callback(tcp_send_callback_t callback);

#endif // LIDAR_PACKET_H