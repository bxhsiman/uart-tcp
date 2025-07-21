#include <stdlib.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_handler.h"
#include "config.h"
#include "lidar_packet.h"

static const char *TAG = "UART_HANDLER";

static void uart_packet_task(void *arg);

void uart_handler_init(void)
{
    config_t *config = config_get();
    
    /* UART配置 - 优化缓冲区大小以处理高速数据 */
    const uart_config_t uc = {
        .baud_rate = config->uart_baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // 大幅增加UART缓冲区大小以避免高速数据丢失
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE*8, UART_BUF_SIZE*4, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_LOGI(TAG, "UART initialized with baud rate: %lu", config->uart_baudrate);
}

void uart_handler_start_task(void)
{
    xTaskCreatePinnedToCore(uart_packet_task, "uart_packet", 8192,
                            NULL, 20, NULL, 1);  // 最高优先级，固定到核心1
}

void uart_write_data(const uint8_t *data, size_t len)
{
    uart_write_bytes(UART_PORT_NUM, (const char *)data, len);
}

static void uart_packet_task(void *arg)
{
    uint8_t *buf = malloc(TCP_SEND_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate UART buffer");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "UART packet processing task started");
    lidar_reset_stats();
    
    for (;;) {
        // 使用较短的超时，保证数据流的连续性
        int len = uart_read_bytes(UART_PORT_NUM, buf, TCP_SEND_BUF_SIZE, pdMS_TO_TICKS(10));
        if (len > 0) {
            // 分析UART数据并自动发送检测到的LiDAR包
            lidar_analyze_data(buf, len);
        }
    }
    free(buf);
}