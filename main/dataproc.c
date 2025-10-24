#include "dataproc.h"
#include "config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/uart.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "cJSON.h"
#include "freertos/projdefs.h"
#include "mbedtls/base64.h"

static const char *TAG = "DATAPROC";

QueueHandle_t g_frame_queue;
QueueHandle_t g_uart_event_queue;

/* 统计信息 */
static uint32_t total_frames_sent = 0;
static uint32_t valid_frames = 0;
static uint32_t invalid_frames = 0;


bool validate_lidar_packet(const uint8_t* data, int len) {
    if (len != LIDAR_PACKET_SIZE) return false;
    if (data[0] != LIDAR_HEADER_0 || data[1] != LIDAR_HEADER_1) return false;
    if (data[2] > 7) return false;
    if (data[3] != 0x00) return false;
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

static char* base64_encode(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, data, len);
    char* encoded = malloc(olen + 1);
    if (!encoded) return NULL;
    mbedtls_base64_encode((unsigned char*)encoded, olen, &olen, data, len);
    encoded[olen] = '\0';
    return encoded;
}

static void get_mac_string(char* mac_str, size_t size) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mac_str, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool send_frame(const lidar_frame_t* frame) {
    xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
    int sock = g_sock;
    xSemaphoreGive(g_sock_mutex);

    if (sock < 0) return false;

    char mac_str[18];
    get_mac_string(mac_str, sizeof(mac_str));

    uint8_t* frame_data = (uint8_t*)&frame->packets[0];

    char* b64_payload = base64_encode(frame_data, LIDAR_FRAME_SIZE);
    if (!b64_payload) return false;

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mac", mac_str);
    cJSON_AddNumberToObject(json, "len", LIDAR_FRAME_SIZE);
    cJSON_AddStringToObject(json, "payload", b64_payload);

    char* json_str = cJSON_PrintUnformatted(json);
    free(b64_payload);
    cJSON_Delete(json);

    if (!json_str) return false;

    int json_len = strlen(json_str);
    int sent = 0;
    bool success = true;

    while (sent < json_len) {
        int ret = send(sock, json_str + sent, json_len - sent, 0);
        if (ret <= 0) {
            success = false;
            xSemaphoreTake(g_sock_mutex, portMAX_DELAY);
            if (g_sock == sock) {
                shutdown(sock, SHUT_RDWR);
                close(sock);
                g_sock = -1;
            }
            xSemaphoreGive(g_sock_mutex);
            break;
        }
        sent += ret;
    }

    free(json_str);
    if (success) {
        total_frames_sent++;
    }
    return success;
}

void init_data_processing(void) {
    g_frame_queue = xQueueCreate(FRAME_BUFFER_COUNT, sizeof(lidar_frame_t));
}

/* UART事件处理任务（中断驱动模式） */
void uart_event_task(void *arg)
{
    uart_event_t event;
    uint8_t *uart_buf = malloc(UART_BUF_SIZE);

    static uint8_t packet_buf[LIDAR_PACKET_SIZE];
    static int packet_pos = 0;
    static lidar_frame_t current_frame;
    static int frame_packet_count = 0;
    static bool frame_started = false;
    static bool header_found = false;

    for (;;) {
        // 等待UART事件（阻塞，由中断驱动）
        if (xQueueReceive(g_uart_event_queue, &event, portMAX_DELAY)) {
            switch (event.type) {
                case UART_DATA:
                    // 有数据到达，立即读取
                    int len = uart_read_bytes(UART_PORT_NUM, uart_buf, event.size, 0);

                    if (len > 0) {
                        for (int i = 0; i < len; i++) {
                            uint8_t byte = uart_buf[i];

                            if (!header_found) {
                                if (packet_pos == 0 && byte == LIDAR_HEADER_0) {
                                    packet_buf[packet_pos++] = byte;
                                } else if (packet_pos == 1 && byte == LIDAR_HEADER_1) {
                                    packet_buf[packet_pos++] = byte;
                                    header_found = true;
                                } else {
                                    packet_pos = 0;
                                    if (byte == LIDAR_HEADER_0) {
                                        packet_buf[packet_pos++] = byte;
                                    }
                                }
                            } else {
                                packet_buf[packet_pos++] = byte;

                                if (packet_pos >= LIDAR_PACKET_SIZE) {
                                    if (validate_lidar_packet(packet_buf, LIDAR_PACKET_SIZE)) {
                                        uint8_t seq = packet_buf[2];

                                        if (seq == 0) {
                                            frame_started = true;
                                            frame_packet_count = 0;
                                            memset(&current_frame, 0, sizeof(current_frame));
                                        }

                                        if (frame_started && seq < LIDAR_FRAME_PACKETS) {
                                            if (seq == frame_packet_count) {
                                                memcpy(&current_frame.packets[seq], packet_buf, LIDAR_PACKET_SIZE);
                                                frame_packet_count++;

                                                if (frame_packet_count == LIDAR_FRAME_PACKETS) {
                                                    current_frame.valid = validate_frame(&current_frame);
                                                    current_frame.timestamp = xTaskGetTickCount();

                                                    if (current_frame.valid) {
                                                        valid_frames++;
                                                        xQueueSend(g_frame_queue, &current_frame, 0);
                                                    } else {
                                                        invalid_frames++;
                                                    }

                                                    frame_started = false;
                                                    frame_packet_count = 0;
                                                }
                                            } else {
                                                if (seq == 0) {
                                                    frame_started = true;
                                                    frame_packet_count = 0;
                                                    memset(&current_frame, 0, sizeof(current_frame));
                                                    memcpy(&current_frame.packets[0], packet_buf, LIDAR_PACKET_SIZE);
                                                    frame_packet_count = 1;
                                                } else {
                                                    frame_started = false;
                                                    frame_packet_count = 0;
                                                }
                                            }
                                        }
                                    }

                                    packet_pos = 0;
                                    header_found = false;
                                }
                            }
                        }
                    }
                    break;

                case UART_FIFO_OVF:
                    LOG_E(TAG, "UART FIFO overflow");
                    // FIFO溢出，清空缓冲区
                    uart_flush_input(UART_PORT_NUM);
                    xQueueReset(g_uart_event_queue);
                    break;

                case UART_BUFFER_FULL:
                    LOG_E(TAG, "UART buffer full ");
                    // 驱动缓冲区满，清空
                    uart_flush_input(UART_PORT_NUM);
                    xQueueReset(g_uart_event_queue);
                    break;

                case UART_BREAK:
                    break;
                case UART_PARITY_ERR:
                    break;
                case UART_FRAME_ERR:
                    LOG_E(TAG, "UART frame error");
                    uart_flush_input(UART_PORT_NUM);
                    break;

                default:
                    break;
            }
        }
    }

    free(uart_buf);
}

void tcp_send_task(void *arg)
{
    lidar_frame_t frame;

    for (;;) {
        if (xQueueReceive(g_frame_queue, &frame, portMAX_DELAY) == pdTRUE) {
            while (g_sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            send_frame(&frame);
        }
    }
}

/* 统计信息获取函数 */
uint32_t get_total_frames_sent(void) {
    return total_frames_sent;
}

uint32_t get_valid_frames(void) {
    return valid_frames;
}

uint32_t get_invalid_frames(void) {
    return invalid_frames;
}

uint32_t get_total_bytes_sent(void) {
    return 0;
}
