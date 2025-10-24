#include "dataproc.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/uart.h"
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "DATAPROC";

/* 帧队列（生产者-消费者模型） */
QueueHandle_t g_frame_queue;

/* 全局统计数据 */
static uint32_t g_total_frames_sent = 0;
static uint32_t g_valid_frames_count = 0;
static uint32_t g_invalid_frames_count = 0;
static uint32_t g_total_bytes_sent = 0;

bool validate_lidar_packet(const uint8_t* data, int len) {
    if (len != LIDAR_PACKET_SIZE) return false;
    if (data[0] != LIDAR_HEADER_0 || data[1] != LIDAR_HEADER_1) return false;
    if (data[2] > 7) return false;  // sequence should be 0-7
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

/* Base64编码辅助函数 */
static char* base64_encode(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, data, len);
    char* encoded = malloc(olen + 1);
    if (!encoded) return NULL;
    mbedtls_base64_encode((unsigned char*)encoded, olen, &olen, data, len);
    encoded[olen] = '\0';
    return encoded;
}

/* 获取MAC地址字符串 */
static void get_mac_string(char* mac_str, size_t size) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mac_str, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* 发送单个帧（由消费者任务调用） */
bool send_frame(const lidar_frame_t* frame) {
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    int sock = g_sock;
    xSemaphoreGive(g_sock_mutex);

    if (sock < 0) {
        LOG_W(TAG, "⚠️  TCP连接未建立，跳过发送");
        return false;
    }

    char mac_str[18];
    get_mac_string(mac_str, sizeof(mac_str));

    uint8_t* frame_data = (uint8_t*)&frame->packets[0];

    LOG_D(TAG, "📋 准备发送帧 (原始大小: %d字节)", LIDAR_FRAME_SIZE);

    // Base64编码
    char* b64_payload = base64_encode(frame_data, LIDAR_FRAME_SIZE);
    if (!b64_payload) {
        LOG_E(TAG, "❌ Base64编码失败");
        return false;
    }

    // 构建JSON对象
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mac", mac_str);
    cJSON_AddNumberToObject(json, "len", LIDAR_FRAME_SIZE);
    cJSON_AddStringToObject(json, "payload", b64_payload);

    char* json_str = cJSON_PrintUnformatted(json);
    free(b64_payload);
    cJSON_Delete(json);

    if (!json_str) {
        LOG_E(TAG, "❌ JSON序列化失败");
        return false;
    }

    int json_len = strlen(json_str);
    int sent = 0;
    bool success = true;

    while (sent < json_len) {
        int ret = send(sock, json_str + sent, json_len - sent, 0);
        if (ret <= 0) {
            LOG_E(TAG, "❌ 发送失败: ret=%d, errno=%d", ret, errno);
            success = false;

            // 关闭失效的socket并标记
            xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
            if (g_sock == sock) {
                shutdown(sock, SHUT_RDWR);
                close(sock);
                g_sock = -1;
                LOG_W(TAG, "🔌 Socket发送失败,已关闭并清理g_sock");
            }
            xSemaphoreGive(g_sock_mutex);
            break;
        }
        sent += ret;
        LOG_D(TAG, "📡 已发送 %d/%d 字节", sent, json_len);
    }

    free(json_str);

    if (success && sent == json_len) {
        g_total_frames_sent++;
        g_total_bytes_sent += LIDAR_FRAME_SIZE;
        LOG_D(TAG, "✅ 帧发送成功 (JSON: %d字节, 原始: %d字节)", json_len, LIDAR_FRAME_SIZE);
    }

    return success;
}

void init_data_processing(void) {
    // 创建帧队列，队列深度为FRAME_BUFFER_COUNT
    g_frame_queue = xQueueCreate(FRAME_BUFFER_COUNT, sizeof(lidar_frame_t));
    if (g_frame_queue == NULL) {
        LOG_E(TAG, "❌ 帧队列创建失败!");
    } else {
        LOG_I(TAG, "✅ 帧队列创建成功 (深度: %d)", FRAME_BUFFER_COUNT);
    }
}

/* 生产者任务：UART接收和帧组装 */
void uart_rx_task(void *arg)
{
    uint8_t *uart_buf = malloc(UART_BUF_SIZE);
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
    static uint32_t queue_full_drops = 0;

    LOG_I(TAG, "🚀 UART接收任务启动 (生产者) - 开始监听UART数据...");
    uint32_t last_time = 0;
    for (;;) {
        // 从UART读取数据
        size_t buffer_len = 0;
        uart_get_buffered_data_len(UART_PORT_NUM, &buffer_len);
        if (buffer_len >= 127) { LOG_E(TAG, "buffer data too long!!!"); }

        int len = uart_read_bytes(UART_PORT_NUM, uart_buf, UART_BUF_SIZE, 5);

        if (len > 0) {
            total_bytes_received += len;
            LOG_D(TAG, "📥 UART接收: %d字节 (总计: %u字节)", len, (unsigned int)total_bytes_received);

            // 打印原始数据 for debug
            // if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
            //     int print_len = (len > 16) ? 16 : len;
            //     char hex_str[64] = {0};
            //     for (int j = 0; j < print_len; j++) {
            //         snprintf(hex_str + j*3, 4, "%02X ", uart_buf[j]);
            //     }
            //     LOG_D(TAG, "📦 原始数据: %s%s", hex_str, len > 16 ? "..." : "");
            // }

            for (int i = 0; i < len; i++) {
                uint8_t byte = uart_buf[i];

                // 如果还没有找到包头，寻找0A 00序列
                if (!header_found) {
                    if (packet_pos == 0 && byte == LIDAR_HEADER_0) {
                        packet_buf[packet_pos++] = byte;
                    } else if (packet_pos == 1 && byte == LIDAR_HEADER_1) {
                        packet_buf[packet_pos++] = byte;
                        header_found = true;
                        LOG_D(TAG, "🎯 找到包头 0A 00");
                    } else {
                        // 重置搜索
                        if (packet_pos > 0) {
                            discarded_bytes += packet_pos;
                            packet_pos = 0;
                        }
                        discarded_bytes++;
                        // 检查当前字节是否是新包头的开始
                        if (byte == LIDAR_HEADER_0) {
                            packet_buf[packet_pos++] = byte;
                        }
                    }
                } else {
                    // 已找到包头，继续填充数据
                    packet_buf[packet_pos++] = byte;

                    // 检查是否收满一个完整包
                    if (packet_pos >= LIDAR_PACKET_SIZE) {
                        total_packets_processed++;

                        // 打印完整包数据测试
                        // if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
                        //     char full_packet_str[140] = {0};
                        //     for (int k = 0; k < LIDAR_PACKET_SIZE; k++) {
                        //         snprintf(full_packet_str + k*3, 4, "%02X ", packet_buf[k]);
                        //     }
                        //     LOG_D(TAG, "📋 完整数据包#%u: %s", (unsigned int)total_packets_processed, full_packet_str);
                        // }

                        if (validate_lidar_packet(packet_buf, LIDAR_PACKET_SIZE)) {
                            valid_packets++;
                            uint8_t seq = packet_buf[2];
                            LOG_D(TAG, "✅ 有效数据包 序列号=%d (有效包: %u/%u)", seq, (unsigned int)valid_packets, (unsigned int)total_packets_processed);

                            // 检查是否是帧开始
                            if (seq == 0) {
                                if (frame_started && frame_packet_count > 0) {
                                    invalid_frames++;
                                    LOG_W(TAG, "❌ 上一帧未完成 (只收到%d个包) - 丢弃", frame_packet_count);
                                }

                                frame_started = true;
                                frame_packet_count = 0;
                                memset(&current_frame, 0, sizeof(current_frame));
                                LOG_D(TAG, "🎬 新帧开始 - 序列0检测到");
                            }

                            if (frame_started && seq < LIDAR_FRAME_PACKETS) {
                                if (seq == frame_packet_count) {
                                    memcpy(&current_frame.packets[seq], packet_buf, LIDAR_PACKET_SIZE);
                                    frame_packet_count++;
                                    LOG_D(TAG, "📝 帧数据包%d已保存 (帧进度: %d/8)", seq, frame_packet_count);

                                    // 检查是否收满一帧
                                    if (frame_packet_count == LIDAR_FRAME_PACKETS) {
                                        current_frame.valid = validate_frame(&current_frame);
                                        current_frame.timestamp = xTaskGetTickCount();

                                        if (current_frame.valid) {
                                            valid_frames++;
                                            g_valid_frames_count++;
                                            LOG_I(TAG, "🎯 完整有效帧#%u 已组装完成!", (unsigned int)valid_frames);

                                            // 发送到队列（非阻塞）
                                            if (xQueueSend(g_frame_queue, &current_frame, 0) != pdTRUE) {
                                                queue_full_drops++;
                                                LOG_I(TAG, "⚠️  队列已满，帧被丢弃 (累计丢弃: %u)", (unsigned int)queue_full_drops);
                                            } else {
                                                LOG_D(TAG, "💾 帧已加入队列 (队列占用: %u/%u)",
                                                      (unsigned int)uxQueueMessagesWaiting(g_frame_queue),
                                                      (unsigned int)FRAME_BUFFER_COUNT);
                                            }
                                        } else {
                                            invalid_frames++;
                                            g_invalid_frames_count++;
                                            LOG_W(TAG, "❌ 帧校验失败#%u - 数据不一致", (unsigned int)invalid_frames);
                                        }

                                        frame_started = false;
                                        frame_packet_count = 0;
                                    }
                                } else {
                                    LOG_W(TAG, "❌ 序列错误: 期望%d, 收到%d - 重置帧", frame_packet_count, seq);
                                    if (seq == 0) {
                                        frame_started = true;
                                        frame_packet_count = 0;
                                        memset(&current_frame, 0, sizeof(current_frame));
                                        memcpy(&current_frame.packets[0], packet_buf, LIDAR_PACKET_SIZE);
                                        frame_packet_count = 1;
                                        LOG_D(TAG, "🔄 从序列0重新开始");
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
                            LOG_W(TAG, "❌ 无效数据包 (无效包: %u/%u) - 头部=%02X %02X, 序列=%02X, 保留=%02X",
                                   (unsigned int)invalid_packets, (unsigned int)total_packets_processed,
                                   packet_buf[0], packet_buf[1], packet_buf[2], packet_buf[3]);
                        }

                        // 重置状态，准备寻找下一个包头
                        packet_pos = 0;
                        header_found = false;
                    }
                }
            }

            // 每处理100个包打印一次统计
            if (total_packets_processed % 100 == 0 && total_packets_processed > 0) {
                LOG_I(TAG, "📊 统计信息: 总字节=%u, 包=%u(有效%u), 帧=%u(有效%u), 丢弃字节=%u, 队列丢帧=%u",
                       (unsigned int)total_bytes_received, (unsigned int)total_packets_processed, (unsigned int)valid_packets,
                       (unsigned int)(valid_frames + invalid_frames), (unsigned int)valid_frames, (unsigned int)discarded_bytes, (unsigned int)queue_full_drops);
            }
        }
        // 统计循环时间
        // uint32_t current_time = xTaskGetTickCount();
        // if (current_time - last_time > pdMS_TO_TICKS(2)) {
        //     LOG_E(TAG, "⏱ UART接收循环时间: %u us", (unsigned int)(current_time - last_time) * 1000 * 1000 / configTICK_RATE_HZ);
        // }
        // last_time = xTaskGetTickCount();
    }
    free(uart_buf);
}

/* 消费者任务：TCP发送 */
void tcp_send_task(void *arg)
{
    lidar_frame_t frame;

    LOG_I(TAG, "🚀 TCP发送任务启动 (消费者) - 等待队列数据...");

    for (;;) {
        // 从队列接收帧（阻塞等待）
        if (xQueueReceive(g_frame_queue, &frame, portMAX_DELAY) == pdTRUE) {
            LOG_D(TAG, "📤 从队列取出帧，准备发送...");

            // 等待TCP连接
            while (g_sock < 0) {
                LOG_W(TAG, "⏳ TCP连接未建立，等待连接...");
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            // 发送帧
            bool success = send_frame(&frame);
            if (success) {
                LOG_I(TAG, "✅ 帧发送成功 (累计: %u帧, %u字节)",
                      (unsigned int)g_total_frames_sent, (unsigned int)g_total_bytes_sent);
            } else {
                LOG_W(TAG, "❌ 帧发送失败，等待TCP重连...");
                // 发送失败时，帧已经丢失，等待socket重连
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

/* 统计信息获取函数 */
uint32_t get_total_frames_sent(void) {
    return g_total_frames_sent;
}

uint32_t get_valid_frames(void) {
    return g_valid_frames_count;
}

uint32_t get_invalid_frames(void) {
    return g_invalid_frames_count;
}

uint32_t get_total_bytes_sent(void) {
    return g_total_bytes_sent;
}
