#include <string.h>
#include <stdbool.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_tls.h"
#include "mqtt_client.h"

#include "env_state.h"
#include "esp_state.h"
#include "esp_serial.h"

#define GPIO_LOCK_IO       GPIO_NUM_12
#define GPIO_LOCK_PIN_SEL  1ULL<<GPIO_LOCK_IO
#define DEFAULT_BUF_SIZE   1024

#define STORAGE_NAMESPACE  "storage"
#define STORAGE_SERIAL_KEY "serial"
#define STORAGE_SERIAL_LENGTH 8

static EventGroupHandle_t wifi_event_group, mqtt_event_group, env_event_group, esp_event_group, lock_event_group;
static esp_mqtt_client_handle_t client;

static const int CONNECTED_BIT = BIT0;

static const char *TAG = "lock";

extern const uint8_t ca_cert_pem_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_cert_pem_end[] asm("_binary_ca_crt_end");

static const char *STATUS_ONLINE = "online";
static const char *STATUS_OFFLINE = "offline";

static const char *MQTT_DEVICE_TOPIC = "discovery/sensor/espressif";
static const char *MQTT_SET_PATH = "set";
static const char *MQTT_STATE_PATH = "state";
static const char *MQTT_STATUS_PATH = "status";
static const char *MQTT_ATTRS_PATH = "attributes";

static char *MQTT_SET_TOPIC;
static char *MQTT_STATE_TOPIC;
static char *MQTT_STATUS_TOPIC;
static char *MQTT_ATTRS_TOPIC;

static uint8_t serial_data[STORAGE_SERIAL_LENGTH];

gpio_config_t lock_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = GPIO_LOCK_PIN_SEL,
};

esp_state_t esp_state = {};
env_state_t env_state = {
        .io = GPIO_LOCK_IO,
        .unlock = ENV_STATE_LOCK
};

esp_serial_t esp_serial = {
        .namespace = STORAGE_NAMESPACE,
        .key = STORAGE_SERIAL_KEY,
        .data = serial_data,
        .length = STORAGE_SERIAL_LENGTH
};

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    char topic[DEFAULT_BUF_SIZE];
    char data[DEFAULT_BUF_SIZE];

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "[MQTT] Connected");
            xEventGroupSetBits(mqtt_event_group, CONNECTED_BIT);
            esp_mqtt_client_subscribe(event->client, MQTT_SET_TOPIC, 0);
            esp_mqtt_client_publish(event->client, MQTT_STATUS_TOPIC, STATUS_ONLINE, 0, 0, true);
            xEventGroupSetBits(env_event_group, CONNECTED_BIT);
            xEventGroupSetBits(esp_event_group, CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            xEventGroupClearBits(mqtt_event_group, CONNECTED_BIT);
            ESP_LOGI(TAG, "[MQTT] Disconnected");
            break;
        case MQTT_EVENT_DATA:
            memset(topic, 0, DEFAULT_BUF_SIZE);
            memset(data, 0, DEFAULT_BUF_SIZE);

            strncpy(topic, event->topic, event->topic_len);
            strncpy(data, event->data, event->data_len);

            ESP_LOGI(TAG, "[MQTT] Data %s with %s", topic, data);

            if (strcmp(topic, MQTT_SET_TOPIC) == 0) {
                env_state_deserialize(&env_state, data);
                env_state_apply(&env_state);

                if (env_state.unlock) {
                    xEventGroupSetBits(lock_event_group, CONNECTED_BIT);
                }

                xEventGroupSetBits(env_event_group, CONNECTED_BIT);
                xEventGroupSetBits(esp_event_group, CONNECTED_BIT);
            }
            break;
        default:
            ESP_LOGV(TAG, "[MQTT] Event ID %d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_start() {
    mqtt_event_group = xEventGroupCreate();
    env_event_group = xEventGroupCreate();
    esp_event_group = xEventGroupCreate();

    const esp_mqtt_client_config_t mqtt_cfg = {
            .uri = CONFIG_MQTT_URI,
            .event_handle = mqtt_event_handler,
            .cert_pem = (const char *) ca_cert_pem_start,
            .lwt_topic = MQTT_STATUS_TOPIC,
            .lwt_msg = STATUS_OFFLINE,
            .lwt_qos = 0,
            .lwt_retain = true,
            .keepalive = 10
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
    ESP_LOGI(TAG, "[MQTT] Connecting to %s...", CONFIG_MQTT_URI);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "[WIFI] Connecting to %s...", CONFIG_WIFI_SSID);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "[WIFI] Connected");
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "[WIFI] Reconnecting to %s...", CONFIG_WIFI_SSID);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;

        default:
            ESP_LOGI(TAG, "[WIFI] Event Base %s with ID %d", base, id);
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "[IP] Got IP:"
                IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } else {
        ESP_LOGI(TAG, "[IP] Event Base %s with ID %d", base, id);
    }
}

static void wifi_start() {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL));

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {
            .sta = {
                    .ssid = CONFIG_WIFI_SSID,
                    .password = CONFIG_WIFI_PASSWORD,
            }
    };

    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}


void ensure_mqtt_topics(void) {
    char *hex;
    esp_serial_hex(&esp_serial, &hex);

    MQTT_SET_TOPIC = malloc(strlen(MQTT_DEVICE_TOPIC) + strlen(hex) + strlen(MQTT_SET_PATH) + 1);
    MQTT_STATE_TOPIC = malloc(strlen(MQTT_DEVICE_TOPIC) + strlen(hex) + strlen(MQTT_STATE_PATH) + 1);
    MQTT_STATUS_TOPIC = malloc(strlen(MQTT_DEVICE_TOPIC) + strlen(hex) + strlen(MQTT_STATUS_PATH) + 1);
    MQTT_ATTRS_TOPIC = malloc(strlen(MQTT_DEVICE_TOPIC) + strlen(hex) + strlen(MQTT_ATTRS_PATH) + 1);

    char *pe = MQTT_SET_TOPIC;
    char *ps = MQTT_STATE_TOPIC;
    char *pt = MQTT_STATUS_TOPIC;
    char *pa = MQTT_ATTRS_TOPIC;

    pe += sprintf(pe, "%s", MQTT_DEVICE_TOPIC);
    ps += sprintf(ps, "%s", MQTT_DEVICE_TOPIC);
    pt += sprintf(pt, "%s", MQTT_DEVICE_TOPIC);
    pa += sprintf(pa, "%s", MQTT_DEVICE_TOPIC);

    pe += sprintf(pe, "/%s/", hex);
    ps += sprintf(ps, "/%s/", hex);
    pt += sprintf(pt, "/%s/", hex);
    pa += sprintf(pa, "/%s/", hex);

    strcpy(pe, MQTT_SET_PATH);
    strcpy(ps, MQTT_STATE_PATH);
    strcpy(pt, MQTT_STATUS_PATH);
    strcpy(pa, MQTT_ATTRS_PATH);

    free(hex);
}

void task_lock_lock(void *param) {
    EventBits_t event_bits;
    lock_event_group = xEventGroupCreate();

    while (true) {
        xEventGroupWaitBits(lock_event_group, CONNECTED_BIT, true, true, portMAX_DELAY);
        vTaskDelay(CONFIG_LOCK_DELAY / portTICK_PERIOD_MS);
        event_bits = xEventGroupGetBits(lock_event_group);
        if (event_bits & CONNECTED_BIT) { continue; }
        if (env_state.unlock) {
            env_state.unlock = ENV_STATE_LOCK;
            env_state_apply(&env_state);
            xEventGroupSetBits(env_event_group, CONNECTED_BIT);
            xEventGroupSetBits(esp_event_group, CONNECTED_BIT);
        }
    }
}

void task_env_publish(void *param) {
    char *data;

    while (true) {
        xEventGroupWaitBits(mqtt_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
        xEventGroupWaitBits(env_event_group, CONNECTED_BIT, true, true, portMAX_DELAY);
        data = env_state_serialize(&env_state);
        ESP_LOGI(TAG, "[MQTT] Publish data %.*s", strlen(data), data);
        esp_mqtt_client_publish(client, MQTT_STATE_TOPIC, data, 0, 0, true);
    }
}

void task_esp_publish(void *param) {
    char *json;

    while (true) {
        xEventGroupWaitBits(mqtt_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
        xEventGroupWaitBits(esp_event_group, CONNECTED_BIT, true, true, portMAX_DELAY);
        esp_state_update(&esp_state);
        json = esp_state_serialize(&esp_state);
        ESP_LOGI(TAG, "[MQTT] Publish data %.*s", strlen(json), json);
        esp_mqtt_client_publish(client, MQTT_ATTRS_TOPIC, json, 0, 0, false);
        free(json);
    }
}

void app_main() {
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_serial_ensure(&esp_serial));

    ensure_mqtt_topics();

    gpio_config(&lock_conf);

    wifi_start();
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    mqtt_start();

    xTaskCreate(task_lock_lock, "task_lock_lock", 2048, NULL, 0, NULL);
    xTaskCreate(task_env_publish, "task_env_publish", 2048, NULL, 0, NULL);
    xTaskCreate(task_esp_publish, "task_esp_publish", 2048, NULL, 0, NULL);
}
