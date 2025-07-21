#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lidar_packet.h"

static const char *TAG = "LIDAR_PACKET";

static lidar_stats_t uart_stats = {0};
static tcp_send_callback_t tcp_send_cb = NULL;
static QueueHandle_t tcp_send_queue = NULL;

#define TCP_SEND_QUEUE_SIZE 100  // 队列大小，可容纳100个批次
#define BATCH_TIMEOUT_MS 100     // 批次超时时间(毫秒)

void lidar_packet_init(void)
{
    lidar_reset_stats();
    
    // 创建TCP批次发送队列
    tcp_send_queue = xQueueCreate(TCP_SEND_QUEUE_SIZE, sizeof(lidar_batch_item_t));
    if (!tcp_send_queue) {
        ESP_LOGE(TAG, "Failed to create TCP batch send queue");
        return;  // 如果队列创建失败，不要继续
    } else {
        ESP_LOGI(TAG, "TCP batch send queue created, size: %d", TCP_SEND_QUEUE_SIZE);
    }
}

void lidar_reset_stats(void)
{
    uart_stats.total_bytes_received = 0;
    uart_stats.packets_detected = 0;
    uart_stats.packets_lost = 0;
    uart_stats.sequence_errors = 0;
    uart_stats.last_sequence = 0;
    uart_stats.first_packet = true;
    uart_stats.last_log_time = 0;
    uart_stats.pending_len = 0;
    
    // 清空批次缓存
    uart_stats.cache_received_mask = 0;
    uart_stats.batches_sent = 0;
    uart_stats.incomplete_batches = 0;
    for (int i = 0; i < LIDAR_BATCH_SIZE; i++) {
        uart_stats.packet_cache[i].received = false;
    }
}

bool lidar_validate_packet(uint8_t *packet)
{
    // 检查包头
    if (packet[0] != LIDAR_HEADER_0 || packet[1] != LIDAR_HEADER_1) {
        return false;
    }
    
    // 检查空字节位置
    if (packet[3] != 0x00) {
        return false;
    }
    
    // 检查序号是否在有效范围内（0-7）
    uint8_t sequence = packet[2] & 0x0F;
    if (sequence > 7) {
        return false;
    }
    
    return true;
}

// 新函数：缓存单个数据包
void lidar_cache_packet(uint8_t *packet)
{
    uint8_t sequence = packet[2] & 0x0F;
    if (sequence >= LIDAR_BATCH_SIZE) {
        ESP_LOGW(TAG, "Invalid sequence number: %d", sequence);
        return;
    }
    
    // 检查是否已经有这个序号的包
    if (uart_stats.packet_cache[sequence].received) {
        ESP_LOGD(TAG, "Duplicate packet sequence %d, replacing", sequence);
    }
    
    // 缓存数据包
    memcpy(uart_stats.packet_cache[sequence].packet, packet, LIDAR_PACKET_SIZE);
    uart_stats.packet_cache[sequence].received = true;
    uart_stats.packet_cache[sequence].timestamp = xTaskGetTickCount();
    
    // 设置对应的位掩码
    uart_stats.cache_received_mask |= (1 << sequence);
    
    ESP_LOGD(TAG, "Cached packet sequence %d, mask: 0x%02X", sequence, (unsigned int)uart_stats.cache_received_mask);
}

// 新函数：检查并发送完整批次
void lidar_check_and_send_batch(void)
{
    uint32_t current_time = xTaskGetTickCount();
    
    // 检查是否收到完整的0-7序列 (掩码应该是0xFF)
    if (uart_stats.cache_received_mask == 0xFF) {
        // 创建批次数据
        lidar_batch_item_t batch_item;
        batch_item.timestamp = current_time;
        batch_item.sequence_mask = uart_stats.cache_received_mask;
        
        // 按序列号0-7拷贝数据
        for (int i = 0; i < LIDAR_BATCH_SIZE; i++) {
            memcpy(batch_item.batch_data + i * LIDAR_PACKET_SIZE, 
                   uart_stats.packet_cache[i].packet, 
                   LIDAR_PACKET_SIZE);
        }
        
        // 发送到TCP队列
        if (tcp_send_queue) {
            if (xQueueSend(tcp_send_queue, &batch_item, 0) == pdTRUE) {
                uart_stats.batches_sent++;
                ESP_LOGI(TAG, "Sent complete batch %lu (8x%d bytes = %d bytes)", 
                         uart_stats.batches_sent, LIDAR_PACKET_SIZE, 
                         LIDAR_BATCH_SIZE * LIDAR_PACKET_SIZE);
            } else {
                ESP_LOGW(TAG, "Failed to send batch to TCP queue (queue full)");
            }
        } else {
            ESP_LOGW(TAG, "TCP send queue not initialized, dropping batch");
        }
        
        // 清空缓存，准备下一批次
        uart_stats.cache_received_mask = 0;
        for (int i = 0; i < LIDAR_BATCH_SIZE; i++) {
            uart_stats.packet_cache[i].received = false;
        }
    } else if (uart_stats.cache_received_mask != 0) {
        // 检查批次超时 - 如果有部分包但超过超时时间，清空缓存
        uint32_t oldest_time = UINT32_MAX;
        for (int i = 0; i < LIDAR_BATCH_SIZE; i++) {
            if (uart_stats.packet_cache[i].received && 
                uart_stats.packet_cache[i].timestamp < oldest_time) {
                oldest_time = uart_stats.packet_cache[i].timestamp;
            }
        }
        
        if (oldest_time != UINT32_MAX && 
            (current_time - oldest_time) > pdMS_TO_TICKS(BATCH_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Batch timeout, clearing incomplete batch (mask: 0x%02X)", 
                     (unsigned int)uart_stats.cache_received_mask);
            uart_stats.incomplete_batches++;
            
            // 清空缓存
            uart_stats.cache_received_mask = 0;
            for (int i = 0; i < LIDAR_BATCH_SIZE; i++) {
                uart_stats.packet_cache[i].received = false;
            }
        }
    }
}

static void process_valid_packet(uint8_t *packet, uint32_t byte_position)
{
    uart_stats.packets_detected++;
    
    // 提取序号（第3个字节的低4位）
    uint8_t sequence = packet[2] & 0x0F;
    
    if (!uart_stats.first_packet) {
        uint8_t expected_seq = (uart_stats.last_sequence + 1) % 8;
        if (sequence != expected_seq) {
            uart_stats.sequence_errors++;
            
            // 修复序列计算逻辑
            int lost;
            if (sequence >= expected_seq) {
                lost = sequence - expected_seq;
            } else {
                lost = (8 - expected_seq) + sequence;
            }
            
            // 更保守的丢包报告策略
            if (lost > 0 && lost <= 4) { // 只报告合理范围内的丢包
                uart_stats.packets_lost += lost;
                ESP_LOGW(TAG, "UART: Sequence jump at byte %lu: expected %d, got %d, lost %d packets", 
                         byte_position, expected_seq, sequence, lost);
            } else if (lost == 0) {
                ESP_LOGD(TAG, "Duplicate packet: sequence %d", sequence);
                return; // 不更新last_sequence，忽略重复包
            } else {
                ESP_LOGD(TAG, "Large sequence gap: expected %d, got %d (possibly corrupted data)", 
                         expected_seq, sequence);
                // 对于大的序列跳跃，我们假设数据流有问题，重新开始
                uart_stats.first_packet = true;
                return;
            }
        }
    } else {
        uart_stats.first_packet = false;
        ESP_LOGI(TAG, "First LiDAR packet detected, sequence: %d", sequence);
    }
    
    uart_stats.last_sequence = sequence;
    
    // 缓存数据包并检查是否可以发送完整批次
    lidar_cache_packet(packet);
    lidar_check_and_send_batch();
}

void lidar_analyze_data(uint8_t *data, int len)
{
    uart_stats.total_bytes_received += len;
    
    // 创建连续的数据流：pending_buffer + 新数据
    uint8_t *combined_buffer = NULL;
    int combined_len = uart_stats.pending_len + len;
    
    if (uart_stats.pending_len > 0) {
        combined_buffer = malloc(combined_len);
        if (!combined_buffer) {
            ESP_LOGE(TAG, "Failed to allocate combined buffer");
            return;
        }
        memcpy(combined_buffer, uart_stats.pending_buffer, uart_stats.pending_len);
        memcpy(combined_buffer + uart_stats.pending_len, data, len);
    } else {
        combined_buffer = data;
    }
    
    // 优化包查找：先查找包头，再验证完整包
    int processed = 0;
    int search_iterations = 0;
    
    for (int i = 0; i <= combined_len - LIDAR_PACKET_SIZE; i++) {
        // 定期让出CPU时间，避免看门狗超时
        if (++search_iterations >= 100) {
            search_iterations = 0;
            taskYIELD();
        }
        
        // 快速包头检查：只有找到包头才进行完整验证
        if (combined_buffer[i] == LIDAR_HEADER_0 && 
            combined_buffer[i + 1] == LIDAR_HEADER_1) {
            
            if (lidar_validate_packet(combined_buffer + i)) {
                // 找到有效包，处理它
                uint32_t absolute_position = uart_stats.total_bytes_received - len + i - uart_stats.pending_len;
                process_valid_packet(combined_buffer + i, absolute_position);
                
                // 跳过整个包，继续查找下一个
                i += LIDAR_PACKET_SIZE - 1;
                processed = i + 1;
            }
        }
    }
    
    // 保存未处理的数据到pending_buffer
    int remaining = combined_len - processed;
    if (remaining > 0 && remaining < LIDAR_PACKET_SIZE) {
        uart_stats.pending_len = remaining;
        memcpy(uart_stats.pending_buffer, combined_buffer + processed, remaining);
    } else {
        uart_stats.pending_len = 0;
    }
    
    // 释放临时缓冲区
    if (combined_buffer != data) {
        free(combined_buffer);
    }
    
    // 每5秒打印一次统计信息
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (current_time - uart_stats.last_log_time > 5000) {
        ESP_LOGI(TAG, "UART Stats: %lu bytes, %lu packets, %lu lost, %lu seq_errors, %lu batches sent (pending: %d, cache_mask: 0x%02X)", 
                 uart_stats.total_bytes_received, uart_stats.packets_detected, 
                 uart_stats.packets_lost, uart_stats.sequence_errors, uart_stats.batches_sent,
                 uart_stats.pending_len, (unsigned int)uart_stats.cache_received_mask);
        uart_stats.last_log_time = current_time;
    }
}

lidar_stats_t* lidar_get_stats(void)
{
    return &uart_stats;
}

void lidar_set_tcp_callback(tcp_send_callback_t callback)
{
    tcp_send_cb = callback;
}

static void tcp_sender_task(void *arg)
{
    lidar_batch_item_t batch_item;
    
    ESP_LOGI(TAG, "TCP sender task started for batch forwarding");
    
    // 等待队列初始化完成
    while (tcp_send_queue == NULL) {
        ESP_LOGW(TAG, "Waiting for TCP send queue initialization...");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "TCP send queue ready, starting batch processing");
    
    while (1) {
        // 等待接收完整批次
        if (xQueueReceive(tcp_send_queue, &batch_item, portMAX_DELAY) == pdTRUE) {
            if (tcp_send_cb) {
                // 直接发送2816字节的批次数据 (8 packets × 352 bytes)
                size_t batch_size = LIDAR_BATCH_SIZE * LIDAR_PACKET_SIZE;
                tcp_send_cb(batch_item.batch_data, batch_size);
                
                ESP_LOGD(TAG, "Sent batch to TCP: %d bytes (mask: 0x%02X)", 
                         batch_size, batch_item.sequence_mask);
            } else {
                ESP_LOGW(TAG, "TCP callback not set, dropping batch");
            }
        }
        
        // 监控队列深度
        UBaseType_t queue_depth = uxQueueMessagesWaiting(tcp_send_queue);
        if (queue_depth > TCP_SEND_QUEUE_SIZE * 0.8) {  // 队列80%满时警告
            static uint32_t last_warning = 0;
            uint32_t current_time = xTaskGetTickCount();
            if (current_time - last_warning > pdMS_TO_TICKS(5000)) {  // 每5秒警告一次
                ESP_LOGW(TAG, "TCP send queue high: %d/%d", queue_depth, TCP_SEND_QUEUE_SIZE);
                last_warning = current_time;
            }
        }
    }
}

void lidar_start_tcp_sender_task(void)
{
    if (!tcp_send_queue) {
        ESP_LOGE(TAG, "Cannot start TCP sender task: queue not initialized");
        return;
    }
    
    xTaskCreatePinnedToCore(tcp_sender_task, "tcp_sender", 8192,
                            NULL, 18, NULL, 0);  // 高优先级，固定到核心0
    ESP_LOGI(TAG, "TCP sender task created with high priority");
}

QueueHandle_t lidar_get_tcp_queue(void)
{
    return tcp_send_queue;
}