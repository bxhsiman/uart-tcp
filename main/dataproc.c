// dataproc.c
#include "dataproc.h"
#include "config.h"

#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

// 兼容不同 IDF 版本：esp_read_mac 在 v5 在 esp_mac.h，老版本在 esp_system.h
#include "esp_mac.h"
#include "esp_system.h"

static const char *TAG = "DATAPROC";

// 由其它模块维护的全局 Socket 与互斥量（这里声明为 extern）
extern int g_sock;
extern SemaphoreHandle_t g_sock_mutex;

/* 全局帧缓冲区 */
lidar_frame_t g_frame_buffer[FRAME_BUFFER_COUNT];
int g_current_frame_index = 0;
int g_buffered_frames = 0;
SemaphoreHandle_t g_frame_mutex;

/* 全局统计数据 */
static uint32_t g_total_frames_sent = 0;
static uint32_t g_valid_frames_count = 0;
static uint32_t g_invalid_frames_count = 0;
static uint32_t g_total_bytes_sent = 0;

/* 每帧要追加的前缀：设备 MAC 的末两字节（raw 2B） */
static uint8_t g_frame_prefix[2] = {0x00, 0x00};

/* ========== 校验相关 ========== */
bool validate_lidar_packet(const uint8_t* data, int len) {
    if (len != LIDAR_PACKET_SIZE) return false;
    if (data[0] != LIDAR_HEADER_0 || data[1] != LIDAR_HEADER_1) return false;
    if (data[2] > 7) return false;      // sequence should be 0-7
    if (data[3] != 0x00) return false;  // reserved byte
    return true;
}

bool validate_frame(const lidar_frame_t* frame) {
    for (int i = 0; i < LIDAR_FRAME_PACKETS; i++) {
        if (frame->packets[i].header[0] != LIDAR_HEADER_0 ||
            frame->packets[i].header[1] != LIDAR_HEADER_1) {
            return false;
        }
        if (frame->packets[i].sequence != i) {
            return false;
        }
        if (frame->packets[i].reserved != 0x00) {
            return false;
        }
    }
    return true;
}

/* ========== 发送辅助函数 ========== */
static bool send_all(int sock, const uint8_t* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int ret = send(sock, buf + sent, len - sent, 0);
        if (ret <= 0) {
            return false;
        }
        sent += ret;
    }
    return true;
}

/* ========== 发送缓冲帧（每帧前拼接 2 字节 MAC 尾） ========== */
bool send_buffered_frames(void) {
    // 取 socket
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    int sock = g_sock;
    xSemaphoreGive(g_sock_mutex);

    if (sock < 0) {
        ESP_LOGW(TAG, "⚠️  TCP连接未建立，跳过发送");
        return false;
    }

    ESP_LOGI(TAG, "📤 开始发送缓冲帧数据 - Socket=%d", sock);

    // 锁住帧缓冲，固定当前要发送的帧数
    xSemaphoreTake(g_frame_mutex, portMAX_DELAY);
    int frames_to_send = g_buffered_frames;
    int successfully_sent = 0;

    for (int i = 0; i < frames_to_send; i++) {
        if (!g_frame_buffer[i].valid) {
            ESP_LOGW(TAG, "⚠️  跳过无效帧#%d", i + 1);
            continue;
        }

        uint8_t* frame_data = (uint8_t*)&g_frame_buffer[i].packets[0];

        // 1) 发送 2 字节前缀（MAC 尾）
        if (!send_all(sock, g_frame_prefix, 2)) {
            ESP_LOGE(TAG, "❌ 发送帧前缀失败, errno=%d", errno);
            continue; // 本帧失败，尝试下一帧
        }

        // 2) 发送完整帧
        int sent = 0;
        while (sent < LIDAR_FRAME_SIZE) {
            int ret = send(sock, frame_data + sent, LIDAR_FRAME_SIZE - sent, 0);
            if (ret <= 0) {
                ESP_LOGE(TAG, "❌ 帧#%d数据发送失败: ret=%d, errno=%d", i + 1, ret, errno);
                break;
            }
            sent += ret;
        }

        if (sent == LIDAR_FRAME_SIZE) {
            successfully_sent++;
            g_total_frames_sent++;
            g_total_bytes_sent += (2 + sent);  // 前缀 + 帧
            ESP_LOGD(TAG, "✅ 帧#%d发送成功 (prefix 2B + %dB)", i + 1, sent);
        } else {
            ESP_LOGE(TAG, "❌ 帧#%d发送不完整 (%d/%d字节)", i + 1, sent, LIDAR_FRAME_SIZE);
        }
    }

    // 清空缓冲区计数
    g_buffered_frames = 0;
    xSemaphoreGive(g_frame_mutex);

    ESP_LOGI(TAG, "🎯 发送完成: %d/%d帧成功 (累计: %u帧)",
             successfully_sent, frames_to_send, (unsigned int)g_total_frames_sent);
    return true;
}

/* ========== 初始化：读取 MAC 并设置帧前缀 ========== */
void init_data_processing(void) {
    g_frame_mutex = xSemaphoreCreateMutex();
    memset(g_frame_buffer, 0, sizeof(g_frame_buffer));
    g_current_frame_index = 0;
    g_buffered_frames = 0;

    // 读取 STA MAC，取末两字节作为前缀
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err == ESP_OK) {
        g_frame_prefix[0] = mac[4];
        g_frame_prefix[1] = mac[5];
        ESP_LOGI(TAG, "Frame prefix (MAC tail) = %02X %02X", mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "读取 MAC 失败 (err=%d), 前缀保持 00 00", err);
    }
}

/* ========== UART -> 帧重组 -> 缓冲 -> 发送 ========== */
void uart_to_sock_task(void *arg)
{
    uint8_t *uart_buf = (uint8_t*)malloc(UART_BUF_SIZE);
    if (!uart_buf) {
        ESP_LOGE(TAG, "❌ 分配 UART 缓冲失败");
        vTaskDelete(NULL);
        return;
    }

    static uint8_t packet_buf[LIDAR_PACKET_SIZE];
    static int packet_pos = 0;
    static lidar_frame_t current_frame;
    static int frame_packet_count = 0;
    static bool frame_started = false;

    static uint32_t total_bytes_received = 0;
    static uint32_t total_packets_processed = 0;
    static uint32_t valid_packets = 0;
    static uint32_t invalid_packets = 0;
    static uint32_t valid_frames = 0;
    static uint32_t invalid_frames = 0;
    static bool header_found = false;
    static uint32_t discarded_bytes = 0;

    ESP_LOGI(TAG, "🚀 UART数据处理任务启动 - 开始监听UART数据...");

    // 阻塞等待 socket 连接
wait_connect:
    ESP_LOGW(TAG, "TCP连接等待中...");
    while (g_sock < 0) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(TAG, "✅ TCP连接已建立 - 开始处理UART数据");

    for (;;) {
        int len = uart_read_bytes(UART_PORT_NUM, uart_buf, UART_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            total_bytes_received += len;
            ESP_LOGD(TAG, "📥 UART接收: %d字节 (总计: %u字节)", len, (unsigned int)total_bytes_received);

            // DEBUG 时打印前 16 字节
            if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
                int print_len = (len > 16) ? 16 : len;
                char hex_str[16 * 3 + 1] = {0};
                for (int j = 0; j < print_len; j++) {
                    snprintf(hex_str + j*3, 4, "%02X ", uart_buf[j]);
                }
                ESP_LOGD(TAG, "📦 原始数据: %s%s", hex_str, len > 16 ? "..." : "");
            }

            for (int i = 0; i < len; i++) {
                uint8_t byte = uart_buf[i];

                // 搜索包头 0A 00
                if (!header_found) {
                    if (packet_pos == 0 && byte == LIDAR_HEADER_0) {
                        packet_buf[packet_pos++] = byte;
                    } else if (packet_pos == 1 && byte == LIDAR_HEADER_1) {
                        packet_buf[packet_pos++] = byte;
                        header_found = true;
                        ESP_LOGD(TAG, "🎯 找到包头 0A 00");
                    } else {
                        // 重置搜索
                        if (packet_pos > 0) {
                            discarded_bytes += packet_pos;
                            packet_pos = 0;
                        }
                        discarded_bytes++;
                        // 检查当前字节是否可能是新包头开始
                        if (byte == LIDAR_HEADER_0) {
                            packet_buf[packet_pos++] = byte;
                        }
                    }
                } else {
                    // 已找到包头，继续收包
                    packet_buf[packet_pos++] = byte;

                    // 收满一个包
                    if (packet_pos >= LIDAR_PACKET_SIZE) {
                        total_packets_processed++;

                        // DEBUG 打印完整包
                        if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
                            char full_packet_str[LIDAR_PACKET_SIZE * 3 + 1];
                            int off = 0;
                            for (int k = 0; k < LIDAR_PACKET_SIZE && off < (int)sizeof(full_packet_str)-4; k++) {
                                off += snprintf(full_packet_str + off, sizeof(full_packet_str) - off, "%02X ", packet_buf[k]);
                            }
                            full_packet_str[sizeof(full_packet_str)-1] = '\0';
                            ESP_LOGD(TAG, "📋 完整数据包#%u: %s", (unsigned int)total_packets_processed, full_packet_str);
                        }

                        if (validate_lidar_packet(packet_buf, LIDAR_PACKET_SIZE)) {
                            valid_packets++;
                            uint8_t seq = packet_buf[2];
                            ESP_LOGD(TAG, "✅ 有效数据包 序列号=%d (有效包: %u/%u)", seq, (unsigned int)valid_packets, (unsigned int)total_packets_processed);

                            // 新帧开始
                            if (seq == 0) {
                                if (frame_started && frame_packet_count > 0) {
                                    invalid_frames++;
                                    ESP_LOGW(TAG, "❌ 上一帧未完成 (只收到%d个包) - 丢弃", frame_packet_count);
                                }
                                frame_started = true;
                                frame_packet_count = 0;
                                memset(&current_frame, 0, sizeof(current_frame));
                                ESP_LOGD(TAG, "🎬 新帧开始 - 序列0检测到");
                            }

                            if (frame_started && seq < LIDAR_FRAME_PACKETS) {
                                if (seq == frame_packet_count) {
                                    memcpy(&current_frame.packets[seq], packet_buf, LIDAR_PACKET_SIZE);
                                    frame_packet_count++;
                                    ESP_LOGD(TAG, "📝 帧数据包%d已保存 (帧进度: %d/%d)", seq, frame_packet_count, LIDAR_FRAME_PACKETS);

                                    // 收满一帧
                                    if (frame_packet_count == LIDAR_FRAME_PACKETS) {
                                        current_frame.valid = validate_frame(&current_frame);
                                        current_frame.timestamp = xTaskGetTickCount();

                                        if (current_frame.valid) {
                                            valid_frames++;
                                            g_valid_frames_count++;
                                            ESP_LOGI(TAG, "🎯 完整有效帧#%u 已组装完成!", (unsigned int)valid_frames);

                                            xSemaphoreTake(g_frame_mutex, portMAX_DELAY);
                                            memcpy(&g_frame_buffer[g_current_frame_index], &current_frame, sizeof(current_frame));
                                            g_current_frame_index = (g_current_frame_index + 1) % FRAME_BUFFER_COUNT;
                                            g_buffered_frames++;
                                            ESP_LOGD(TAG, "💾 帧已缓存 (缓冲区: %d/%d)", g_buffered_frames, FRAME_BUFFER_COUNT);

                                            // 缓冲区满则立即发送
                                            if (g_buffered_frames >= FRAME_BUFFER_COUNT) {
                                                int to_send = g_buffered_frames;
                                                xSemaphoreGive(g_frame_mutex); // 先释放锁，避免在 send 内部长时间持有
                                                ESP_LOGI(TAG, "🚀 缓冲区满，开始发送 %d 帧数据", to_send);
                                                bool ret = send_buffered_frames();
                                                if (!ret) {
                                                    ESP_LOGW(TAG, "⚠️  发送失败，等待TCP连接...");
                                                    goto wait_connect;
                                                }
                                            } else {
                                                xSemaphoreGive(g_frame_mutex);
                                            }
                                        } else {
                                            invalid_frames++;
                                            g_invalid_frames_count++;
                                            ESP_LOGW(TAG, "❌ 帧校验失败#%u - 数据不一致", (unsigned int)invalid_frames);
                                        }

                                        frame_started = false;
                                        frame_packet_count = 0;
                                    }
                                } else {
                                    ESP_LOGW(TAG, "❌ 序列错误: 期望%d, 收到%d - 重置帧", frame_packet_count, seq);
                                    if (seq == 0) {
                                        frame_started = true;
                                        frame_packet_count = 0;
                                        memset(&current_frame, 0, sizeof(current_frame));
                                        memcpy(&current_frame.packets[0], packet_buf, LIDAR_PACKET_SIZE);
                                        frame_packet_count = 1;
                                        ESP_LOGD(TAG, "🔄 从序列0重新开始");
                                    } else {
                                        frame_started = false;
                                        frame_packet_count = 0;
                                        invalid_frames++;
                                        g_invalid_frames_count++;
                                    }
                                }
                            }
                        } else {
                            invalid_packets++;
                            ESP_LOGW(TAG, "❌ 无效数据包 (无效包: %u/%u) - 头部=%02X %02X, 序列=%02X, 保留=%02X",
                                     (unsigned int)invalid_packets, (unsigned int)total_packets_processed,
                                     packet_buf[0], packet_buf[1], packet_buf[2], packet_buf[3]);
                        }

                        // 重置，继续找下一个包
                        packet_pos = 0;
                        header_found = false;
                    }
                }
            }

            // 周期性统计
            if (total_packets_processed % 100 == 0 && total_packets_processed > 0) {
                ESP_LOGI(TAG, "📊 统计信息: 总字节=%u, 包=%u(有效%u), 帧=%u(有效%u), 丢弃字节=%u",
                         (unsigned int)total_bytes_received,
                         (unsigned int)total_packets_processed, (unsigned int)valid_packets,
                         (unsigned int)(valid_frames + invalid_frames), (unsigned int)valid_frames,
                         (unsigned int)discarded_bytes);
            }
        }
    }

    free(uart_buf);
}

/* ========== 统计信息获取函数 ========== */
uint32_t get_total_frames_sent(void)   { return g_total_frames_sent; }
uint32_t get_valid_frames(void)        { return g_valid_frames_count; }
uint32_t get_invalid_frames(void)      { return g_invalid_frames_count; }
uint32_t get_total_bytes_sent(void)    { return g_total_bytes_sent; }
