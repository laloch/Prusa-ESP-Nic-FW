/* UART NIC

  This code makes ESP WiFi device accessible to external system using UART. 
  This is implemented using a simple protocol that enables sending and
  receiveing network packets and some confuration using messages.

  In general the application works as follows:
  - Read incomming messages on UART
  - Read incomming packets on WiFi
  - Resend incomming packets from Wifi as UART messages
  - Resend incomming packet messages from UART using WiFi
  - Configure WiFi interface acoring to client message
  - Report link status on Wifi event or explicit request using UART message

  This aims to be used from MCUs. A Python script that exposes UART nic as
  Linux tap device is attached for testing.


  Copyright (C) 2022 Prusa Research a.s - www.prusa3d.com
  SPDX-License-Identifier: GPL-3.0-or-later 
*/

#define UART_FULL_THRESH_DEFAULT (60)

#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_aio.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp8266/uart_register.h"

#include "esp_private/wifi.h"
#include "esp_supplicant/esp_wpa.h"


// Externals with no header
int ieee80211_output_pbuf(esp_aio_t *aio);
esp_err_t mac_init(void);

static const uint16_t FW_VERSION = 8;

// Hack: because we don't see the beacon on some networks (and it's quite
// common), but don't want to be "flapping", we set the timeout for beacon
// inactivity to a ridiculously long time and handle the disconnect ourselves.
//
// It's not longer for the only reason the uint16_t doesn't hold as big numbers.
static const uint16_t INACTIVE_BEACON_SECONDS = 3600 * 18;
// This is the effective timeout. If we don't receive any packet for this long,
// we consider the signal lost.
//
// TODO: Shall we generate something to provoke getting some packets? Like ARP
// pings to the AP?
static const uint32_t INACTIVE_PACKET_SECONDS = 5;

// intron
// 0 as uint8_t
// fw version as uint16_t
// hw addr data as uint8_t[6]
#define MSG_DEVINFO 0

// intron
// 1 as uint8_t
// link up as bool (uint8_t)
#define MSG_LINK 1

// intron
// 2 as uint8_t
#define MSG_GET_LINK 2

// intron
// 2 as uint8_t
// ssid size as uint8_t
// ssid bytes
// pass size as uint8_t
// pass bytes
#define MSG_CLIENTCONFIG 3

// intron
// 3 as uint8_t
// LEN as uint32_t
// DATA
#define MSG_PACKET 4

// intron
// 5 as uint8_t
// new intron as uint8_t[8]
#define MSG_INTRON 5

#define MSG_START_SOFTAP 6

#define MSG_ALIVE 7

static const uint8_t uart_nic_protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;

static const char *TAG = "uart_nic";

SemaphoreHandle_t uart_mtx = NULL;
static int s_retry_num = 0;
QueueHandle_t uart_tx_queue = 0;
QueueHandle_t wifi_egress_queue = 0;

static char intron[8] = {'U', 'N', '\x00', '\x01', '\x02', '\x03', '\x04', '\x05'};
#define MAC_LEN 6
static uint8_t mac[MAC_LEN];

static uint32_t now_seconds() {
    return xTaskGetTickCount() / configTICK_RATE_HZ;
}

static atomic_uint_least32_t last_inbound_seen = 0;
static atomic_bool associated = false;

static bool beacon_quirk;
static uint8_t probe_max_reties = 3;
static atomic_bool probe_in_progress = false;
static uint8_t probe_retry_count;

static bool ap_silent = false;

typedef struct {
    size_t len;
    void *data;
    void *rx_buff;
} wifi_receive_buff;

typedef struct {
    size_t len;
    void *data;
} wifi_send_buff;

static void IRAM_ATTR free_wifi_receive_buff(wifi_receive_buff *buff) {
    if(buff->rx_buff) esp_wifi_internal_free_rx_buffer(buff->rx_buff);
    if(buff->data) free(buff->data);
    free(buff);
}

static void IRAM_ATTR free_wifi_send_buff(wifi_send_buff *buff) {
    if(buff->data) free(buff->data);
    free(buff);
}

static void send_link_status(uint8_t up) {
    ESP_LOGI(TAG, "Sending link status: %d", up);
    xSemaphoreTake(uart_mtx, portMAX_DELAY);
    uart_write_bytes(UART_NUM_0, intron, sizeof(intron));
    const uint8_t t = MSG_LINK;
    uart_write_bytes(UART_NUM_0, (const char*)&t, 1);
    uart_write_bytes(UART_NUM_0, (const char*)&up, sizeof(uint8_t));
    xSemaphoreGive(uart_mtx);
}

static void probe_task() {
    wifi_scan_config_t config;

    // We need to do full scan, because the ssid/bssid filters don't work
    config.ssid = NULL;
    config.bssid = NULL;
    config.channel = 0;
    config.show_hidden = true;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 120;
    config.scan_time.active.max = 300;
    ESP_ERROR_CHECK(esp_wifi_scan_start(&config, false));

    vTaskDelete(NULL);
}

static void probe_run() {
    xTaskCreate(&probe_task, "probe", 1024, NULL, tskIDLE_PRIORITY + 1, NULL);
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        uint8_t current_protocol;
        ESP_ERROR_CHECK(esp_wifi_get_protocol(ESP_IF_WIFI_STA, &current_protocol));
        if (current_protocol != uart_nic_protocol) {
            ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_STA, uart_nic_protocol));
            return;
        }
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        associated = false;
        send_link_status(0);
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        last_inbound_seen = now_seconds();
        associated = true;
        beacon_quirk = true;
        send_link_status(1);
        s_retry_num = 0;
        ESP_ERROR_CHECK(esp_wifi_set_inactive_time(ESP_IF_WIFI_STA, INACTIVE_BEACON_SECONDS));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_ap_record_t ap_info;
        ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&ap_info));

        const wifi_event_sta_scan_done_t *scan_data = (const wifi_event_sta_scan_done_t *)event_data;
        uint16_t ap_count = scan_data->number;

        bool found = false;
        if (!scan_data->status && ap_count) {
            wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, aps));
            // Try to match BSSID first and if that fails go on and try SSID match . The BSSD check
            // should be sufficient, but there are APs that advertise mismatching BSSID in their
            // beacons and/or probe rensonse. That's the real culprit of the beacon timeout
            // disconnects and the primary motivation of this whole excercise.
            for (int i = 0; i < ap_count; ++i) {
                if (0 == memcmp(ap_info.bssid, aps[i].bssid, 6)) {
                    found = true;
                    beacon_quirk = false;
                    break;
                }
            }
            if (beacon_quirk && !found) {
                for (int i = 0; i < ap_count; ++i) {
                    if (ap_info.ssid && ap_info.ssid[0] && aps[i].ssid && aps[i].ssid[0]) {
                        if (0 == strncmp((char *)(ap_info.ssid), (char *)(aps[i].ssid), 32)) {
                            found = true;
                            break;
                        }
                    }
                }
            }
            free(aps);
        }
        if (!found){
            if (probe_retry_count++ < probe_max_reties) {
                probe_run();
            } else {
                send_link_status(0);
                probe_in_progress = false;
            }
        } else {
            probe_in_progress = false;
            last_inbound_seen = now_seconds();
        }
   }
}


static void send_alive(const uint8_t code) {
    const uint8_t d[2] = {
        MSG_ALIVE, code,
    };
    xSemaphoreTake(uart_mtx, portMAX_DELAY);
    uart_write_bytes(UART_NUM_0, intron, sizeof(intron));
    uart_write_bytes(UART_NUM_0, (const char*)d, 2);
    xSemaphoreGive(uart_mtx);
}

static void ap_task() {
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    uint8_t data[64] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,    //dst MAC
        0, 0, 0, 0, 0, 0,                      //src MAC
        0, 46,                                 //length
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0                                //FCS
    };
    memcpy(&data[6], mac, 6);
    uint32_t count = 0;
    while (true) {
        if (++count % 100 == 0) {
            send_alive(41);
        }
        esp_wifi_internal_tx(ESP_IF_WIFI_AP, data, 64);
        vTaskDelay(2);
    }
}

static void ap_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static TaskHandle_t xHandle = NULL;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        send_alive(40);
        if (!ap_silent)
            xTaskCreate(&ap_task, "softap", 1024, NULL, tskIDLE_PRIORITY + 1, &xHandle);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        if (xHandle) {
            vTaskDelete(xHandle);
            xHandle = NULL;
        }
    }
}

static int IRAM_ATTR wifi_receive_cb(void *buffer, uint16_t len, void *eb) {
    // Seeing some traffic - we have signal :-)
    last_inbound_seen = now_seconds();

    // MAC filter
    if ((((const char*)buffer)[5] & 0x01) == 0) {
        for (uint i = 0; i < 6; ++i) {
            if(((const char*)buffer)[i] != mac[i]) {
                goto cleanup;
            }
        }
    }

    wifi_receive_buff *buff = malloc(sizeof(wifi_receive_buff));
    if(!buff) {
        goto cleanup;
    }
    buff->len = len;
    buff->data = buffer;
    buff->rx_buff = eb;
    if (!xQueueSendToBack(uart_tx_queue, (void *)&buff, (TickType_t)0/*portMAX_DELAY*/)) {
        free_wifi_receive_buff(buff);
    }
    return 0;

cleanup:
    esp_wifi_internal_free_rx_buffer(eb);
    free(buffer);
    return 0;
}

void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(mac_init());
    esp_wifi_set_rx_pbuf_mem_type(WIFI_RX_PBUF_DRAM);
    ESP_ERROR_CHECK(esp_wifi_init_internal(&cfg));
    ESP_ERROR_CHECK(esp_supplicant_init());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, wifi_receive_cb));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

static void send_device_info() {
    ESP_LOGI(TAG, "Sending device info");
    xSemaphoreTake(uart_mtx, portMAX_DELAY);

    // Intron
    uart_write_bytes(UART_NUM_0, intron, sizeof(intron));

    // Definfo mesage identifier
    const uint8_t t = MSG_DEVINFO;
    uart_write_bytes(UART_NUM_0, (const char*)&t, 1);

    // FW version
    uart_write_bytes(UART_NUM_0, (const char*)&FW_VERSION, sizeof(FW_VERSION));

    // MAC address
    int ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if(ret != ESP_OK) {
        ESP_LOGI(TAG, "Failed to obtain MAC, returning last one or zeroes");
    }
    uart_write_bytes(UART_NUM_0, (const char*)mac, sizeof(mac));

    xSemaphoreGive(uart_mtx);
}

static void IRAM_ATTR wait_for_intron() {
    // ESP_LOGI(TAG, "Waiting for intron");
    uint pos = 0;
    while(pos < sizeof(intron)) {
        char c;
        int read = uart_read_bytes(UART_NUM_0, (uint8_t*)&c, 1, portMAX_DELAY);
        if(read == 1) {
            if (c == intron[pos]) {
                pos++;
            } else {
                //ESP_LOGI(TAG, "Invalid: %c, val: %d\n", c, (int)c);
                pos = 0;
            }
        } else {
            ESP_LOGI(TAG, "Timeout!!!");
        }
    }
    // ESP_LOGI(TAG, "Intron found");
}

static void start_softap(uint8_t channel) {
    s_retry_num = CONFIG_ESP_MAXIMUM_RETRY;

    if (channel > 100) {
        ap_silent = true;
        channel -= 100;
    } else {
        ap_silent = false;
    }
    send_alive(10);
    esp_wifi_deinit();

    tcpip_adapter_init();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &ap_event_handler, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "_test",
            .ssid_len = 5,
            .channel = channel,
            .password = "",
            .ssid_hidden = 1,
            .max_connection = 5,
            .authmode = WIFI_AUTH_OPEN,
            .beacon_interval = 100
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    send_alive(20);
}

/**
 * @brief Read data from UART
 * 
 * @param buff Buffer to store the data
 * @param len Number of bytes to read
 * @return size_t Number of bytes actually read
 */
static size_t IRAM_ATTR read_uart(uint8_t *buff, size_t len) {
    size_t trr = 0;
    while(trr < len) {
        int read = uart_read_bytes(UART_NUM_0, ((uint8_t*)buff) + trr, len - trr, portMAX_DELAY);
        if(read < 0) {
            ESP_LOGI(TAG, "Failed to read from UART");
            if(trr != len) {
                ESP_LOGI(TAG, "Read %d != %d expected\n", trr, len);
            }
            return trr;
        }
        trr += read;
    }
    return trr;
}

static void IRAM_ATTR read_packet_message() {
    // ESP_LOGI(TAG, "Reading packet");
    uint32_t size = 0;

    read_uart((uint8_t*)&size, sizeof(size));
    if(size > 2000) {
        ESP_LOGI(TAG, "Invalid packet size: %d", size);
        return;
    }
    // ESP_LOGI(TAG, "Receiving packet size: %d", size);
    // ESP_LOGI(TAG, "Allocating pbuf size: %d, free heap: %d", size, esp_get_free_heap_size());

    wifi_send_buff *buff = malloc(sizeof(wifi_send_buff));
    if(!buff) {
        goto nomem;
    }
    buff->len = size;
    buff->data = malloc(buff->len);
    if(!buff->data) {
        free(buff);
        goto nomem;
    }

    read_uart(buff->data, buff->len);

    if (!xQueueSendToBack(wifi_egress_queue, (void *)&buff, (TickType_t)0/*portMAX_DELAY*/)) {
        ESP_LOGI(TAG, "Out of space in egress queue");
        free_wifi_send_buff(buff);
    }
    return;

nomem:
    ESP_LOGI(TAG, "Out of mem for packet data");
    for(uint i = 0; i < size; ++i) {
        uint8_t c;
        read_uart(&c, 1);
    }
    return;
}

static void read_wifi_client_message() {
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));

    uint8_t ssid_len = 0;
    read_uart(&ssid_len, 1);
    ESP_LOGI(TAG, "Reading SSID len: %d", ssid_len);
    if(ssid_len > sizeof(wifi_config.sta.ssid)) {
        ESP_LOGI(TAG, "SSID too long, trimming");
        ssid_len = sizeof(wifi_config.sta.ssid);
    }
    read_uart(wifi_config.sta.ssid, ssid_len);

    uint8_t pass_len = 0;
    read_uart(&pass_len, 1);
    ESP_LOGI(TAG, "Reading PASS len: %d", pass_len);
    if(pass_len > sizeof(wifi_config.sta.password)) {
        ESP_LOGI(TAG, "PASS too long, trimming");
        pass_len = sizeof(wifi_config.sta.password);
    }
    read_uart(wifi_config.sta.password, pass_len);

    ESP_LOGI(TAG, "Reconfiguring wifi");

    /* Setting a password implies station will connect to all security modes including WEP/WPA.
        * However these modes are deprecated and not advisable to be used. Incase your Access point
        * doesn't support WPA2, these mode can be enabled by commenting below line */
    if (strlen((char *)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());
    send_device_info();
}

static void IRAM_ATTR read_intron_message() {
    read_uart((uint8_t*)intron, sizeof(intron));
}

static void read_softap_message() {
    uint8_t channel;
    read_uart(&channel, 1);
    if (channel == 255) {
        esp_restart();
    } else {
        start_softap(channel);
    }
}

static int get_link_status() {
    static wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    // ap_info is not important, just not receiven ESP_ERR_WIFI_NOT_CONNECT means we are associated
    const bool online = ret == ESP_OK;
    associated = online;
    return online;
}

static void check_online_status() {
    if (!associated || probe_in_progress) {
        // Nothing to check, we are not online and we know it.
        return;
    }
    const uint32_t last = last_inbound_seen; // Atomic load
    const uint32_t now = now_seconds();
    // The time may overflow from time to time and due to the conversion to
    // seconds, we don't know when exactly. But if it overflows, the now would
    // get smaller than the last time (assuming we check often enough). In that
    // case we ignore the part up to the overflow and take just the part in the
    // „new round“.
    const uint32_t elapsed = now >= last ? now - last : now;

    if (elapsed > INACTIVE_PACKET_SECONDS) {
        probe_in_progress = true;
        probe_retry_count = 0;
        probe_run();
    }
}

static void IRAM_ATTR read_message() {
    wait_for_intron();

    // Check that we are receiving some packets from the AP. We do so in the
    // thread that receives messages from the main CPU because we know that one
    // will generate a message from time to time (at least the get-link one).
    // On the other hand, if we lose connectivity, we will receive no packets
    // from the AP and we would block forever and never get to the check.
    check_online_status();

    uint8_t type = 0;
    size_t read = uart_read_bytes(UART_NUM_0, (uint8_t*)&type, 1, portMAX_DELAY);
    if(read != 1) {
        ESP_LOGI(TAG, "Cannot read message type");
        return;
    }

    // ESP_LOGI(TAG, "Detected message type: %d", type);
    if(type == MSG_PACKET) {
        read_packet_message();
    } else if (type == MSG_CLIENTCONFIG) {
        read_wifi_client_message();
    } else if (type == MSG_GET_LINK) {
        send_link_status(get_link_status());
    } else if (type == MSG_INTRON) {
        read_intron_message();
    } else if (type == MSG_START_SOFTAP) {
        send_alive(99);
        read_softap_message();
    } else {
        ESP_LOGI(TAG, "Unknown message type: %d !!!", type);
    }
}

static void IRAM_ATTR wifi_egress_thread(void *arg) {
    for(;;) {
        wifi_send_buff *buff;
        if(xQueueReceive(wifi_egress_queue, &buff, (TickType_t)portMAX_DELAY)) {
            if (!buff) {
                ESP_LOGI(TAG, "NULL pulled from egress queue");
                continue;
            }

            int8_t err = esp_wifi_internal_tx(ESP_IF_WIFI_STA, buff->data, buff->len);
            if (err != ESP_OK) {
                ESP_LOGI(TAG, "Failed to send packet !!!");
            }
            free_wifi_send_buff(buff);
        }
    }
}

static void IRAM_ATTR output_rx_thread(void *arg) {
    ESP_LOGI(TAG, "Started RX thread");
    for(;;) {
        read_message();
    }
}

static void IRAM_ATTR uart_tx_thread(void *arg) {
    // Send initial device info to let master know ESP is ready
    send_device_info();

    for(;;) {
        wifi_receive_buff *buff;
        if(xQueueReceive(uart_tx_queue, &buff, (TickType_t)1000 /*portMAX_DELAY*/)) {
            if (!buff) {
                continue;
            }
            //ESP_LOGI(TAG, "Printing packet to UART");
            xSemaphoreTake(uart_mtx, portMAX_DELAY);
            uart_write_bytes(UART_NUM_0, intron, sizeof(intron));
            const uint8_t t = MSG_PACKET;
            const uint32_t l = buff->len;
            uart_write_bytes(UART_NUM_0, (const char*)&t, sizeof(t));
            uart_write_bytes(UART_NUM_0, (const char*)&l, sizeof(l));
            uart_write_bytes(UART_NUM_0, (const char*)buff->data, buff->len);
            xSemaphoreGive(uart_mtx);
            //ESP_LOGI(TAG, "Packet UART out done");
            free_wifi_receive_buff(buff);
        }
    }
}

void app_main() {
    ESP_LOGI(TAG, "UART NIC");

	esp_log_level_set("*", ESP_LOG_ERROR);

    ESP_ERROR_CHECK(nvs_flash_init());

    // Configure parameters of an UART driver,
    // communication pins and install the driver
    uart_config_t uart_config = {
        .baud_rate = 4600000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_NUM_0, 16384, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);
    uart_intr_config_t uart_intr = {
        .intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M
        | UART_RXFIFO_TOUT_INT_ENA_M
        | UART_FRM_ERR_INT_ENA_M
        | UART_RXFIFO_OVF_INT_ENA_M,
        .rxfifo_full_thresh = 80,
        .rx_timeout_thresh = 1,
        .txfifo_empty_intr_thresh = 40
    };
    uart_intr_config(UART_NUM_0, &uart_intr);

    ESP_LOGI(TAG, "UART RE-INITIALIZED");

    uart_mtx = xSemaphoreCreateMutex();
    if (!uart_mtx) {
        ESP_LOGI(TAG, "Could not create UART mutex");
        return;
    }

    uart_tx_queue = xQueueCreate(20, sizeof(wifi_receive_buff*));
    if (uart_tx_queue == 0) {
        ESP_LOGI(TAG, "Failed to create INPUT/TX queue");
        return;
    }

    wifi_egress_queue = xQueueCreate(20, sizeof(wifi_send_buff*));
    if (wifi_egress_queue == 0) {
        ESP_LOGI(TAG, "Failed to create WiFi TX queue");
        return;
    }

    ESP_LOGI(TAG, "Wifi init");
    esp_wifi_restore();
    wifi_init_sta();
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "Creating RX thread");
    xTaskCreate(&output_rx_thread, "output_rx_thread", 2048, NULL, 1, NULL);
    ESP_LOGI(TAG, "Creating WiFi-out thread");
    xTaskCreate(&wifi_egress_thread, "wifi_egress_thread", 2048, NULL, 12, NULL);
    ESP_LOGI(TAG, "Creating TX thread");
    xTaskCreate(&uart_tx_thread, "uart_tx_thread", 2048, NULL, 14, NULL);
}
