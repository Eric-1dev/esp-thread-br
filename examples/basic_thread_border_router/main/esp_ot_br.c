/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 #include <stdio.h>
 #include <string.h>
 #include "sdkconfig.h"
 #include "esp_check.h"
 #include "esp_err.h"
 #include "esp_event.h"
 #include "esp_log.h"
 #include "esp_netif.h"
 #include "esp_openthread.h"
 #include "esp_openthread_border_router.h"
 #include "esp_openthread_types.h"
 #include "esp_ot_config.h"
 #include "esp_ot_ota_commands.h"
 #include "esp_ot_wifi_cmd.h"
 #include "esp_spiffs.h"
 #include "esp_vfs_eventfd.h"
 #include "mdns.h"
 #include "nvs_flash.h"
 #include "driver/gpio.h"
 #include "driver/uart.h"
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "freertos/semphr.h"
 #include <ctype.h>
 #include "esp_wifi.h"
 #include "esp_br_web.h"
 #include "esp_http_server.h"
 #include "border_router_launch.h"
 #if CONFIG_EXTERNAL_COEX_ENABLE
 #include "esp_coexist.h"
 #endif
 
 #define TAG "esp_ot_br"
 #define RESET_BUTTON_GPIO GPIO_NUM_0
 #define RESET_HOLD_TIME_MS 3000
 #define DEFAULT_SCAN_LIST_SIZE 10
 #define WIFI_CONNECT_TIMEOUT_MS 30000
 
 extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
 extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
 
 static SemaphoreHandle_t wifi_connect_semaphore = NULL;
 static bool wifi_connect_success = false;
 
 static esp_err_t init_spiffs(void)
 {
     #if CONFIG_AUTO_UPDATE_RCP
     esp_vfs_spiffs_conf_t rcp_fw_conf = {
         .base_path = "/" CONFIG_RCP_PARTITION_NAME,
         .partition_label = CONFIG_RCP_PARTITION_NAME,
         .max_files = 10,
         .format_if_mount_failed = false
     };
     ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&rcp_fw_conf), TAG, "Failed to mount rcp firmware storage");
     #endif
     #if CONFIG_OPENTHREAD_BR_START_WEB
     esp_vfs_spiffs_conf_t web_server_conf = {
         .base_path = "/spiffs",
         .partition_label = "web_storage",
         .max_files = 10,
         .format_if_mount_failed = false
     };
     ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&web_server_conf), TAG, "Failed to mount web storage");
     #endif
     return ESP_OK;
 }
 
 void url_decode(const char *src, char *dst)
 {
     char a, b;
     while (*src) {
         if ((*src == '%') &&
             ((a = src[1]) && (b = src[2])) &&
             (isxdigit(a) && isxdigit(b))) {
             if (a >= 'a') a -= 'a' - 'A';
             if (a >= 'A') a -= ('A' - 10);
             else a -= '0';
             if (b >= 'a') b -= 'a' - 'A';
             if (b >= 'A') b -= ('A' - 10);
             else b -= '0';
             *dst++ = 16 * a + b;
             src += 3;
         } else if (*src == '+') {
             *dst++ = ' ';
             src++;
         } else {
             *dst++ = *src++;
         }
     }
     *dst++ = '\0';
 }
 
 static bool check_reset_button(void)
 {
     gpio_config_t io_conf = {
         .pin_bit_mask = (1ULL << RESET_BUTTON_GPIO),
         .mode = GPIO_MODE_INPUT,
         .pull_up_en = GPIO_PULLUP_ENABLE,
         .pull_down_en = GPIO_PULLDOWN_DISABLE,
         .intr_type = GPIO_INTR_DISABLE,
     };
     gpio_config(&io_conf);
 
     ESP_LOGI(TAG, "Checking reset button");
 
     int held_time = 0;
 
     while (held_time < RESET_HOLD_TIME_MS) {
         if (gpio_get_level(RESET_BUTTON_GPIO) == 0)
             return true;
 
         vTaskDelay(pdMS_TO_TICKS(100));
         held_time += 100;
     }
 
     return false;
 }
 
 static void reset_wifi_credentials(void)
 {
     nvs_handle_t nvs_handle;
     if (nvs_open("wifi_config", NVS_READWRITE, &nvs_handle) == ESP_OK) {
         nvs_erase_all(nvs_handle);
         nvs_commit(nvs_handle);
         nvs_close(nvs_handle);
     }
 }
 
 static bool wifi_credentials_exist(void)
 {
     nvs_handle_t nvs_handle;
     esp_err_t err = nvs_open("wifi_config", NVS_READONLY, &nvs_handle);
     if (err != ESP_OK) {
         return false;
     }
 
     size_t required_size;
     err = nvs_get_str(nvs_handle, "ssid", NULL, &required_size);
     nvs_close(nvs_handle);
     return (err == ESP_OK && required_size > 1);
 }
 
 static void wifi_start_ap(void)
 {
     esp_netif_create_default_wifi_ap();
     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
     ESP_ERROR_CHECK(esp_wifi_init(&cfg));
     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
 
     wifi_config_t ap_config = {
         .ap = {
             .ssid = "OTBR_Setup",
             .ssid_len = strlen("OTBR_Setup"),
             .channel = 1,
             .max_connection = 4,
             .authmode = WIFI_AUTH_OPEN
         }
     };
 
     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
     ESP_ERROR_CHECK(esp_wifi_start());
 }
 
 static void wifi_connect_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
 {
    ESP_LOGI(TAG, "Connected to WiFi");
    if (wifi_connect_semaphore) {
        wifi_connect_success = true;
        xSemaphoreGive(wifi_connect_semaphore);
    }
 }
 
 static esp_err_t wifi_scan_handler(httpd_req_t *req)
 {
     wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
     uint16_t ap_count = DEFAULT_SCAN_LIST_SIZE;
     ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
     ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
 
     if (ap_count > DEFAULT_SCAN_LIST_SIZE) {
         ap_count = DEFAULT_SCAN_LIST_SIZE;
     }
 
     ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_info));
 
     char response[1024] = {0};
     strcat(response, "[");
     size_t response_len = 1;
     for (int i = 0; i < ap_count; i++) {
         char item[256];
         snprintf(item, sizeof(item), "{\"ssid\":\"%s\",\"rssi\":%d}%s",
                  ap_info[i].ssid, ap_info[i].rssi, i < ap_count - 1 ? "," : "");
         if (response_len + strlen(item) < sizeof(response) - 2) {
             strcat(response, item);
             response_len += strlen(item);
         } else {
             ESP_LOGW(TAG, "Response buffer too small, truncating network list");
             break;
         }
     }
     strcat(response, "]");
 
     httpd_resp_set_type(req, "application/json");
     httpd_resp_sendstr(req, response);
     return ESP_OK;
 }
 
 static esp_err_t wifi_connect_handler(httpd_req_t *req)
 {
     char decoded_ssid[32];
     char decoded_password[64];
 
     {
         char buf[512];
         int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
         if (ret <= 0) {
             httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
             return ESP_FAIL;
         }
 
         buf[ret] = '\0';
 
         char *ssid = strstr(buf, "ssid=");
         if (!ssid) {
             httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
             return ESP_FAIL;
         }
         ssid += 5;
         char *ssid_end = strchr(ssid, '&');
         if (!ssid_end) {
             httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID format");
             return ESP_FAIL;
         }
         *ssid_end = 0;
 
         char *password = strstr(ssid_end + 1, "password=");
         if (!password) {
             httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing password");
             return ESP_FAIL;
         }
         password += 9;
 
         url_decode(ssid, decoded_ssid);
         url_decode(password, decoded_password);
     }
 
     wifi_config_t wifi_config = {
         .sta = {
             .ssid = "",
             .password = "",
         },
     };
 
     strncpy((char*)wifi_config.sta.ssid, decoded_ssid, sizeof(wifi_config.sta.ssid));
     strncpy((char*)wifi_config.sta.password, decoded_password, sizeof(wifi_config.sta.password));
 
     // Disconnect from current network if connected
     esp_err_t err = esp_wifi_disconnect();
     if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
         ESP_LOGW(TAG, "Failed to disconnect from current network: %s", esp_err_to_name(err));
     }
 
     // Create semaphore for synchronization
     wifi_connect_semaphore = xSemaphoreCreateBinary();
     if (!wifi_connect_semaphore) {
         httpd_resp_set_type(req, "application/json");
         httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Не удалось создать семафор\"}");
         return ESP_FAIL;
     }
 
     wifi_connect_success = false;
 
     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
     err = esp_wifi_connect();
 
     if (err != ESP_OK) {
         xSemaphoreGive(wifi_connect_semaphore);
         vSemaphoreDelete(wifi_connect_semaphore);
         wifi_connect_semaphore = NULL;
         httpd_resp_set_type(req, "application/json");
         httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Не удалось инициировать подключение\"}");
         return ESP_FAIL;
     }
 
     // Wait for connection result with timeout
     if (xSemaphoreTake(wifi_connect_semaphore, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)) == pdTRUE) {
         vSemaphoreDelete(wifi_connect_semaphore);
         wifi_connect_semaphore = NULL;
 
         if (wifi_connect_success) {
             // Save credentials
             nvs_handle_t nvs_handle;
             ESP_ERROR_CHECK(nvs_open("wifi_config", NVS_READWRITE, &nvs_handle));
             ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ssid", decoded_ssid));
             ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "password", decoded_password));
             ESP_ERROR_CHECK(nvs_commit(nvs_handle));
             nvs_close(nvs_handle);
 
             httpd_resp_set_type(req, "application/json");
             httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Подключение успешно. Настройки сохранены. Перезагрузите устройство\"}");
         } else {
             httpd_resp_set_type(req, "application/json");
             httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Не удалось подключиться к указанной сети.\"}");
         }
     } else {
         // Timeout reached
         esp_wifi_disconnect();
         vSemaphoreDelete(wifi_connect_semaphore);
         wifi_connect_semaphore = NULL;
         httpd_resp_set_type(req, "application/json");
         httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Не удалось подключиться: превышено время ожидания.\"}");
     }
 
     return ESP_OK;
 }
 
 static esp_err_t reboot_handler(httpd_req_t *req)
 {
     httpd_resp_set_type(req, "application/json");
     httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"Устройство перезагружается...\"}");
 
     // Задержка для отправки ответа перед перезагрузкой
     vTaskDelay(pdMS_TO_TICKS(1000));
     esp_restart();
 
     return ESP_OK;
 }
 
 static esp_err_t wifi_config_get_handler(httpd_req_t *req)
 {
     const char *html =
         "<!DOCTYPE html><html><head><title>настройка Wi-Fi</title><meta charset=\"UTF-8\">"
         "<style>"
         "    body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background: #f0f2f5; }"
         "    .container { background: white; padding: 30px; border-radius: 10px; box-shadow: 0px 0px 10px rgba(0,0,0,0.1); display: inline-block; }"
         "    input, select { padding: 10px; margin: 10px 0; border-radius: 5px; border: 1px solid #ccc; }"
         "    .network-selection { display: flex; align-items: center; justify-content: center; }"
         "    select { width: 220px; }"
         "    button { color: white; border: none; padding: 10px; margin: 10px; border-radius: 5px; cursor: pointer; }"
         "    .green-btn { background: #4CAF50; }"
         "    .green-btn:hover { background: #45a049; }"
         "    .red-btn { background: #ff4d4d; }"
         "    .red-btn:hover { background: #e60000; }"
         "    .refresh-btn { width: 40px; height: 40px; padding: 0; border: 1px solid #ccc; background: none; cursor: pointer; }"
         "    .refresh-btn:hover { background: #f5f5f5; }"
         "    .loader { display: none; border: 4px solid #f3f3f3; border-top: 4px solid #4CAF50; border-radius: 50%; width: 24px; height: 24px; animation: spin 1s linear infinite; margin: 10px; }"
         "    .connect-loader, .reboot-loader { display: none; border: 4px solid #f3f3f3; border-top: 4px solid #4CAF50; border-radius: 50%; width: 24px; height: 24px; animation: spin 1s linear infinite; margin: 10px auto; }"
         "    @keyframes spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }"
         "    #message { display: none; margin: 10px; padding: 10px; border-radius: 5px; }"
         "    #message.success { background: #dff0d8; color: #3c763d; }"
         "    #message.error { background: #f2dede; color: #a94442; }"
         "</style></head>"
         "<body>"
         "    <div class='container'>"
         "        <h1>Настройка WiFi-соединения для OpenThread Border Router</h1>"
         "        <div class='network-selection'>"
         "            <select id='networks' onchange='selectNetwork()'>"
         "                <option value=''>Выберите сеть...</option>"
         "            </select>"
         "            <button class='refresh-btn' id='refresh-btn' onclick='scanNetworks()'>"
         "                <svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='#4CAF50' stroke-width='2'>"
         "                    <path d='M21 12a9 9 0 11-6.22-8.66M21 12v-4h-4' />"
         "                </svg>"
         "            </button>"
         "            <div class='loader' id='refresh-loader'></div>"
         "        </div>"
         "        <input type='text' id='ssid' placeholder='Имя сети (SSID)' required><br>"
         "        <input type='text' id='password' placeholder='Пароль' required><br>"
         "        <div>"
         "          <button id='connect-btn' class='green-btn' onclick='connect()'>Подключиться</button>"
         "          <div class='connect-loader' id='connect-loader'></div>"
         "        </div>"
         "        <div>"
         "          <button id='reboot-btn' class='red-btn' onclick='reboot()'>Перезагрузить</button>"
         "          <div class='reboot-loader' id='reboot-loader'></div>"
         "        </div>"
         "        <div id='message'></div>"
         "    </div>"
         "    <script>"
         "        function showLoader(buttonId, show) {"
         "            const btn = document.getElementById(buttonId);"
         "            const loaderId = buttonId === 'refresh-btn' ? 'refresh-loader' : buttonId === 'connect-btn' ? 'connect-loader' : 'reboot-loader';"
         "            const loader = document.getElementById(loaderId);"
         "            btn.style.display = show ? 'none' : 'inline-block';"
         "            loader.style.display = show ? 'inline-block' : 'none';"
         "        }"
         "        function showMessage(message, isSuccess) {"
         "            const msgDiv = document.getElementById('message');"
         "            msgDiv.textContent = message;"
         "            msgDiv.className = isSuccess ? 'success' : 'error';"
         "            msgDiv.style.display = 'block';"
         "            setTimeout(() => { msgDiv.style.display = 'none'; }, 5000);"
         "        }"
         "        function scanNetworks() {"
         "            showLoader('refresh-btn', true);"
         "            fetch('/scan').then(response => response.json()).then(data => {"
         "                showLoader('refresh-btn', false);"
         "                const select = document.getElementById('networks');"
         "                select.innerHTML = '<option value=\"\">Выберите сеть...</option>';"
         "                data.forEach(network => {"
         "                    const option = document.createElement('option');"
         "                    option.value = network.ssid;"
         "                    option.textContent = `${network.ssid} (${network.rssi}dBm)`;"
         "                    select.appendChild(option);"
         "                });"
         "            }).catch(err => {"
         "                showLoader('refresh-btn', false);"
         "                showMessage('Ошибка при сканировании сетей', false);"
         "            });"
         "        }"
         "        function selectNetwork() {"
         "            const select = document.getElementById('networks');"
         "            const ssidInput = document.getElementById('ssid');"
         "            ssidInput.value = select.value;"
         "        }"
         "        function connect() {"
         "            const ssid = document.getElementById('ssid').value;"
         "            const password = document.getElementById('password').value;"
         "            if (!ssid || !password) {"
         "                showMessage('Введите SSID и пароль', false);"
         "                return;"
         "            }"
         "            showLoader('connect-btn', true);"
         "            fetch('/connect', {"
         "                method: 'POST',"
         "                headers: {'Content-Type': 'application/x-www-form-urlencoded'},"
         "                body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`"
         "            }).then(response => response.json()).then(data => {"
         "                showLoader('connect-btn', false);"
         "                showMessage(data.message, data.status === 'success');"
         "            }).catch(err => {"
         "                showLoader('connect-btn', false);"
         "                showMessage('Роутер ответил ошибкой', false);"
         "            });"
         "        }"
         "        function reboot() {"
         "            showLoader('reboot-btn', true);"
         "            fetch('/reboot', {"
         "                method: 'POST',"
         "                headers: {'Content-Type': 'application/x-www-form-urlencoded'},"
         "                body: ''"
         "            }).then(response => response.json()).then(data => {"
         "                showLoader('reboot-btn', false);"
         "                showMessage(data.message, data.status === 'success');"
         "            }).catch(err => {"
         "                showLoader('reboot-btn', false);"
         "                showMessage('Ошибка при перезагрузке', false);"
         "            });"
         "        }"
         "        window.onload = scanNetworks;"
         "    </script>"
         "</body></html>";
 
     httpd_resp_set_type(req, "text/html");
     httpd_resp_sendstr(req, html);
     return ESP_OK;
 }
 
 static httpd_handle_t start_http_server(void)
 {
     httpd_config_t config = HTTPD_DEFAULT_CONFIG();
     httpd_handle_t server = NULL;
     if (httpd_start(&server, &config) == ESP_OK) {
         httpd_uri_t get_uri = {
             .uri = "/",
             .method = HTTP_GET,
             .handler = wifi_config_get_handler,
             .user_ctx = NULL
         };
         httpd_register_uri_handler(server, &get_uri);
 
         httpd_uri_t scan_uri = {
             .uri = "/scan",
             .method = HTTP_GET,
             .handler = wifi_scan_handler,
             .user_ctx = NULL
         };
         httpd_register_uri_handler(server, &scan_uri);
 
         httpd_uri_t connect_uri = {
             .uri = "/connect",
             .method = HTTP_POST,
             .handler = wifi_connect_handler,
             .user_ctx = NULL
         };
         httpd_register_uri_handler(server, &connect_uri);
 
         httpd_uri_t reboot_uri = {
             .uri = "/reboot",
             .method = HTTP_POST,
             .handler = reboot_handler,
             .user_ctx = NULL
         };
         httpd_register_uri_handler(server, &reboot_uri);
     }
     return server;
 }
 
 #if CONFIG_EXTERNAL_COEX_ENABLE
 static void ot_br_external_coexist_init(void)
 {
     esp_external_coex_gpio_set_t gpio_pin = ESP_OPENTHREAD_DEFAULT_EXTERNAL_COEX_CONFIG();
     esp_external_coex_set_work_mode(EXTERNAL_COEX_LEADER_ROLE);
     ESP_ERROR_CHECK(esp_enable_extern_coex_gpio_pin(CONFIG_EXTERNAL_COEX_WIRE_TYPE, gpio_pin));
 }
 #endif
 
 void app_main(void)
 {
     esp_vfs_eventfd_config_t eventfd_config = {
         .max_fds = 4,
     };
 
     esp_openthread_platform_config_t platform_config = {
         .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
         .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
         .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
     };
     esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();
 
     ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
     ESP_ERROR_CHECK(nvs_flash_init());
     ESP_ERROR_CHECK(init_spiffs());
     ESP_ERROR_CHECK(esp_netif_init());
     ESP_ERROR_CHECK(esp_event_loop_create_default());
 
     #if CONFIG_EXTERNAL_COEX_ENABLE
     ot_br_external_coexist_init();
     #endif
 
     ESP_ERROR_CHECK(mdns_init());
     ESP_ERROR_CHECK(mdns_hostname_set("esp-ot-br"));
     #if CONFIG_OPENTHREAD_CLI_OTA
     esp_set_ota_server_cert((char *)server_cert_pem_start);
     #endif
 
     if (check_reset_button()) {
         ESP_LOGI(TAG, "Reset button was pressed. Clearing Wi-Fi settings.");
         reset_wifi_credentials();
     }
 
     if (wifi_credentials_exist()) {
         #if CONFIG_OPENTHREAD_BR_START_WEB
         esp_br_web_start("/spiffs");
         #endif
         launch_openthread_border_router(&platform_config, &rcp_update_config);
     } else {
         ESP_LOGI(TAG, "Starting AP.");
         ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &wifi_connect_event_handler, NULL));
         wifi_start_ap();
         start_http_server();
     }
 }