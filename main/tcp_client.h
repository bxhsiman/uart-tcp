#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/semphr.h"

/* TCP客户端管理函数 */
void tcp_client_init(void);
void tcp_client_start_task(void);
void tcp_client_send_data(uint8_t *data, size_t len);
bool tcp_client_is_connected(void);
int tcp_client_get_socket(void);
SemaphoreHandle_t tcp_client_get_mutex(void);

/* UART数据接收回调函数类型 */
typedef void (*uart_data_callback_t)(const uint8_t *data, size_t len);
void tcp_client_set_uart_callback(uart_data_callback_t callback);

#endif // TCP_CLIENT_H