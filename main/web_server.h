#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"

/* Web服务器管理函数 */
httpd_handle_t web_server_start(void);
void web_server_stop(httpd_handle_t server);

#endif // WEB_SERVER_H