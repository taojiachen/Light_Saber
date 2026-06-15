#ifndef APP_TCP_H
#define APP_TCP_H

void tcp_server_init(void);
int tcp_send_data(const char *data, int len);
void tcp_register_key_task(TaskHandle_t handle);

#endif