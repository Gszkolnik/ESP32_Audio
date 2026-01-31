#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

// Inicjalizacja i zatrzymanie serwera
esp_err_t web_server_init(void);
esp_err_t web_server_stop(void);

// Czy serwer działa
bool web_server_is_running(void);

// WebSocket - wysyłanie aktualizacji stanu
esp_err_t web_server_send_state_update(const char *json_state);

#endif // WEB_SERVER_H
