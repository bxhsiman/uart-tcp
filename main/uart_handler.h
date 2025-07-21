#ifndef UART_HANDLER_H
#define UART_HANDLER_H

#include <stdint.h>
#include "driver/uart.h"

/* UART处理器初始化和控制函数 */
void uart_handler_init(void);
void uart_handler_start_task(void);
void uart_write_data(const uint8_t *data, size_t len);

#endif // UART_HANDLER_H