#include "ble.h"
#include "broadcasters.h"
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_gatt_defs.h>
#include <esp_gatts_api.h>
#include <esp_gatt_common_api.h>
#include <esp_err.h>
#include <esp_log.h>
#include <unistd.h>
#include <string.h>
#include <esp_bt_defs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <endian.h>
#include <string.h>
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "config.h"
#include "wifi.h"
#include <esp_wifi.h>
#include <cJSON.h>

#include "sdkconfig.h"

#define GATTS_TAG "GATTS_DEMO"
/* Constants */
#define INVALID_HANDLE 0

static const char *TAG = "BLE";
static esp_ble_scan_params_t ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    /* Scan interval. This is defined as the time interval from
     * when the Controller started its last LE scan until it begins the
     * subsequent LE scan.
     * Range: 0x0004 to 0x4000 Default: 0x0010 (10 ms)
     * Time = N * 0.625 msec
     * Time Range: 2.5 msec to 10.24 seconds
     */
    .scan_interval = 0x80, /* 128 * 0.625ms = 80ms */
    /* Scan window. The duration of the LE scan. LE_Scan_Window shall be
     * less than or equal to LE_Scan_Interval
     * Range: 0x0004 to 0x4000 Default: 0x0010 (10 ms)
     * Time = N * 0.625 msec
     * Time Range: 2.5 msec to 10240 msec
     */
    .scan_window = 0x10, /* 16 * 0.625ms = 10ms */
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

/* Types */
typedef enum
{
    BLE_OPERATION_TYPE_CONNECT,
    BLE_OPERATION_TYPE_READ,
    BLE_OPERATION_TYPE_WRITE,
    BLE_OPERATION_TYPE_WRITE_NR,
    BLE_OPERATION_TYPE_WRITE_CHAR,
} ble_operation_type_t;

typedef struct ble_operation_t
{
    struct ble_operation_t *next;
    ble_operation_type_t type;
    ble_device_t *device;
    ble_characteristic_t *characteristic;
    size_t len;
    uint8_t *value;
} ble_operation_t;

/* Internal state */
static uint8_t scan_requested = 0;
static esp_gatt_if_t g_gattc_if = ESP_GATT_IF_NONE;
static ble_device_t *devices_list = NULL;
static ble_operation_t *operation_queue = NULL;
static TimerHandle_t purge_device_list_timer = NULL;
static SemaphoreHandle_t devices_list_semaphore = NULL;

/* Callback functions */
static ble_on_broadcaster_discovered_cb_t on_broadcaster_discovered_cb = NULL;
static ble_on_device_discovered_cb_t on_device_discovered_cb = NULL;
static ble_on_device_connected_cb_t on_device_connected_cb = NULL;
static ble_on_device_disconnected_cb_t on_device_disconnected_cb = NULL;
static ble_on_device_services_discovered_cb_t
    on_device_services_discovered_cb = NULL;
static ble_on_device_characteristic_value_cb_t
    on_device_characteristic_value_cb = NULL;
static ble_on_passkey_requested_cb_t on_passkey_requested_cb = NULL;

/// Declare the static function
// static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
// static void gatts_profile_b_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

#define GATTS_SERVICE_UUID_TEST_A 0x00FF
#define GATTS_CHAR_UUID_TEST_A 0xFF01
#define GATTS_DESCR_UUID_TEST_A 0x3333
#define GATTS_NUM_HANDLE_TEST_A 4

#define GATTS_SERVICE_UUID_TEST_B 0x00EE
#define GATTS_CHAR_UUID_TEST_B 0xEE01
#define GATTS_DESCR_UUID_TEST_B 0x2222
#define GATTS_NUM_HANDLE_TEST_B 4

#define TEST_DEVICE_NAME "SMART_THERMOMETER"
#define TEST_MANUFACTURER_DATA_LEN 17

#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0x40

#define PREPARE_BUF_MAX_SIZE 1024

static uint8_t char1_str[] = {0x11, 0x22, 0x33};
static esp_gatt_char_prop_t a_property = 0;
static esp_gatt_char_prop_t b_property = 0;

static esp_attr_value_t gatts_demo_char1_val =
    {
        .attr_max_len = GATTS_DEMO_CHAR_VAL_LEN_MAX,
        .attr_len = sizeof(char1_str),
        .attr_value = char1_str,
};

static uint8_t adv_config_done = 0;
#define adv_config_flag (1 << 0)
#define scan_rsp_config_flag (1 << 1)

#ifdef CONFIG_SET_RAW_ADV_DATA
static uint8_t raw_adv_data[] = {
    0x02, 0x01, 0x06,
    0x02, 0x0a, 0xeb, 0x03, 0x03, 0xab, 0xcd};
static uint8_t raw_scan_rsp_data[] = {
    0x0f, 0x09, 0x45, 0x53, 0x50, 0x5f, 0x47, 0x41, 0x54, 0x54, 0x53, 0x5f, 0x44,
    0x45, 0x4d, 0x4f};
#else

static uint8_t adv_service_uuid128[32] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    // first uuid, 16bit, [12],[13] is the value
    0xfb,
    0x34,
    0x9b,
    0x5f,
    0x80,
    0x00,
    0x00,
    0x80,
    0x00,
    0x10,
    0x00,
    0x00,
    0xEE,
    0x00,
    0x00,
    0x00,
    // second uuid, 32bit, [12], [13], [14], [15] is the value
    0xfb,
    0x34,
    0x9b,
    0x5f,
    0x80,
    0x00,
    0x00,
    0x80,
    0x00,
    0x10,
    0x00,
    0x00,
    0xFF,
    0x00,
    0x00,
    0x00,
};

// The length of adv data must be less than 31 bytes
// static uint8_t test_manufacturer[TEST_MANUFACTURER_DATA_LEN] =  {0x12, 0x23, 0x45, 0x56};
// adv data
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006, // slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval = 0x0010, // slave connection max interval, Time = max_interval * 1.25 msec
    .appearance = 0x00,
    .manufacturer_len = 0,       // TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL, //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid128),
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
// scan response data
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    //.min_interval = 0x0006,
    //.max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,       // TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL, //&test_manufacturer[0],
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid128),
    .p_service_uuid = adv_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

#endif /* CONFIG_SET_RAW_ADV_DATA */

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    //.peer_addr            =
    //.peer_addr_type       =
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

#define PROFILE_NUM 2
#define PROFILE_A_APP_ID 0
#define PROFILE_B_APP_ID 1
struct gatts_profile_inst
{
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};
// /* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
// static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
//     [PROFILE_A_APP_ID] = {
//         .gatts_cb = gatts_profile_a_event_handler,
//         .gatts_if = ESP_GATT_IF_NONE, /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
//     },
//     [PROFILE_B_APP_ID] = {
//         .gatts_cb = gatts_profile_b_event_handler, /* This demo does not implement, similar as profile A */
//         .gatts_if = ESP_GATT_IF_NONE,              /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
//     },
// };

typedef struct
{
    uint8_t *prepare_buf;
    int prepare_len;
} prepare_type_env_t;

static prepare_type_env_t a_prepare_write_env;
static prepare_type_env_t b_prepare_write_env;
void example_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);
void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param);

// end avd
void ble_set_on_broadcaster_discovered_cb(ble_on_broadcaster_discovered_cb_t cb)
{
    on_broadcaster_discovered_cb = cb;
}

void ble_set_on_device_discovered_cb(ble_on_device_discovered_cb_t cb)
{
    on_device_discovered_cb = cb;
}

void ble_set_on_device_connected_cb(ble_on_device_connected_cb_t cb)
{
    on_device_connected_cb = cb;
}

void ble_set_on_device_disconnected_cb(ble_on_device_disconnected_cb_t cb)
{
    on_device_disconnected_cb = cb;
}

void ble_set_on_device_services_discovered_cb(
    ble_on_device_services_discovered_cb_t cb)
{
    on_device_services_discovered_cb = cb;
}

void ble_set_on_device_characteristic_value_cb(
    ble_on_device_characteristic_value_cb_t cb)
{
    on_device_characteristic_value_cb = cb;
}

void ble_set_on_passkey_requested_cb(ble_on_passkey_requested_cb_t cb)
{
    on_passkey_requested_cb = cb;
}

/* BLE Queue */
static void ble_operation_remove_by_type(ble_operation_t **queue,
                                         ble_operation_type_t type)
{
    ble_operation_t *tmp, **cur = queue;

    while (*cur)
    {
        if ((*cur)->type == type)
        {
            tmp = *cur;
            *cur = (*cur)->next;
            if (tmp->len)
                free(tmp->value);
            free(tmp);
        }
        else
            cur = &(*cur)->next;
    }
}

static void ble_operation_remove_by_mac(ble_operation_t **queue,
                                        mac_addr_t mac)
{
    ble_operation_t *tmp, **cur = queue;

    while (*cur)
    {
        if (ble_mac_equal((*cur)->device->mac, mac))
        {
            tmp = *cur;
            *cur = (*cur)->next;
            if (tmp->len)
                free(tmp->value);
            free(tmp);
        }
        else
            cur = &(*cur)->next;
    }
}

static inline void ble_operation_perform(ble_operation_t *operation)
{
    switch (operation->type)
    {
    case BLE_OPERATION_TYPE_CONNECT:
        /* Stop scanning while attempting to connect */
        esp_ble_gap_stop_scanning();
        esp_ble_gattc_open(g_gattc_if, operation->device->mac,
                           operation->device->addr_type, true);
        break;
    case BLE_OPERATION_TYPE_READ:
        esp_ble_gattc_read_char(g_gattc_if, operation->device->conn_id,
                                operation->characteristic->handle, ESP_GATT_AUTH_REQ_NONE);
        break;
    case BLE_OPERATION_TYPE_WRITE:
        esp_ble_gattc_write_char(g_gattc_if, operation->device->conn_id,
                                 operation->characteristic->handle, operation->len, operation->value,
                                 ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        break;
    case BLE_OPERATION_TYPE_WRITE_NR:
        esp_ble_gattc_write_char(g_gattc_if, operation->device->conn_id,
                                 operation->characteristic->handle, operation->len, operation->value,
                                 ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        break;
    case BLE_OPERATION_TYPE_WRITE_CHAR:
        esp_ble_gattc_write_char_descr(g_gattc_if, operation->device->conn_id,
                                       operation->characteristic->client_config_handle, operation->len,
                                       operation->value,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        break;
    }
}

static void ble_operation_dequeue(ble_operation_t **queue)
{
    ble_operation_t *operation = *queue;

    /* Queue is empty, nothing to do */
    if (!operation)
        return;

    *queue = operation->next;
    ESP_LOGD(TAG, "Dequeue: type: %d, device: " MAC_FMT ", char: " UUID_FMT ", "
                  "len: %u, val: %p",
             operation->type, MAC_PARAM(operation->device->mac),
             UUID_PARAM(operation->characteristic->uuid), operation->len,
             operation->value);
    ble_operation_perform(operation);

    if (operation->type == BLE_OPERATION_TYPE_WRITE_CHAR)
        ble_operation_dequeue(queue);

    if (operation->len)
        free(operation->value);
    free(operation);
}

static void ble_queue_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGD(TAG, "Queue timer expired");
    ble_operation_dequeue(&operation_queue);
}

static void ble_operation_enqueue(ble_operation_t **queue,
                                  ble_operation_type_t type, ble_device_t *device,
                                  ble_characteristic_t *characteristic, size_t len, const uint8_t *value)
{
    static TimerHandle_t timer = NULL;
    static ble_characteristic_t null_characteristic;
    ble_operation_t **iter, *operation = malloc(sizeof(*operation));

    operation->next = NULL;
    operation->type = type;
    operation->device = device;
    operation->characteristic = characteristic ?: &null_characteristic;
    operation->len = len;
    if (len)
    {
        operation->value = malloc(len);
        memcpy(operation->value, value, len);
    }
    else
        operation->value = NULL;

    ESP_LOGD(TAG, "Enqueue: type: %d, device: " MAC_FMT ", char: " UUID_FMT ", "
                  "len: %u, val: %p",
             operation->type, MAC_PARAM(operation->device->mac),
             UUID_PARAM(operation->characteristic->uuid), operation->len,
             operation->value);

    for (iter = queue; *iter; iter = &(*iter)->next)
        ;
    *iter = operation;

    /* Create timer */
    if (timer == NULL)
    {
        timer = xTimerCreate("ble_queue", pdMS_TO_TICKS(500), pdFALSE, NULL,
                             ble_queue_timer_cb);
    }

    /* First item in queue or timer is already running, reset timer */
    if (!(*queue)->next || xTimerIsTimerActive(timer))
        xTimerReset(timer, 0);
}

void ble_clear_bonding_info(void)
{
    int i, dev_num = esp_ble_get_bond_device_num();

    esp_ble_bond_dev_t *dev_list = (esp_ble_bond_dev_t *)malloc(
        sizeof(esp_ble_bond_dev_t) * dev_num);

    esp_ble_get_bond_device_list(&dev_num, dev_list);
    for (i = 0; i < dev_num; i++)
        esp_ble_remove_bond_device(dev_list[i].bd_addr);

    /* Block until all bonding information is removed */
    while (esp_ble_get_bond_device_num())
        vTaskDelay(500 / portTICK_PERIOD_MS);

    free(dev_list);
}

int ble_scan_start(void)
{
    ESP_LOGI(TAG, "Starting BLE scan %d", scan_requested);
    if (scan_requested)
        return 0;

    scan_requested = 1;
    return esp_ble_gap_start_scanning(-1);
}

int ble_scan_stop(void)
{
    ESP_LOGI(TAG, "Stopping BLE scan");
    if (!scan_requested)
        return 0;

    ble_operation_remove_by_type(&operation_queue, BLE_OPERATION_TYPE_CONNECT);

    scan_requested = 0;
    return esp_ble_gap_stop_scanning();
}

ble_device_t *get_ble_device(mac_addr_t mac)
{

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    return ble_device_find_by_mac(devices_list, mac);
}
int ble_connect(mac_addr_t mac)
{
    ble_device_t *device;
    int ret = -1;

    // ESP_LOGI(TAG, "test");
    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);
    device = ble_device_find_by_mac(devices_list, mac);
    if (!(device = ble_device_find_by_mac(devices_list, mac)))
        goto Exit;

    // ESP_LOGI(TAG, "%s", device->name);
    // char *name;

    // name = strstr(device->name, "JXB-TTM");
    // if (name == NULL)
    //     goto Exit;
    // {
    // ESP_LOGI(TAG, "Discovered BLE device: " MAC_FMT " (RSSI: %d), not connecting",
    //          MAC_PARAM(device->mac), rssi);
    // }
    // ESP_LOGI(TAG, "Discovered BLE device: " MAC_FMT " (RSSI: %d), connecting",
    //          MAC_PARAM(device->mac), rssi);

    ble_operation_enqueue(&operation_queue, BLE_OPERATION_TYPE_CONNECT, device,
                          NULL, 0, NULL);

    ret = 0;

Exit:
    xSemaphoreGiveRecursive(devices_list_semaphore);
    return ret;
}

static int _ble_disconnect(ble_device_t *dev)
{
    /* Remove queued requests for this device */
    ble_operation_remove_by_mac(&operation_queue, dev->mac);
    return esp_ble_gattc_close(g_gattc_if, dev->conn_id);
}

int ble_disconnect(mac_addr_t mac)
{
    ble_device_t *dev;
    int ret = -1;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    if (!(dev = ble_device_find_by_mac(devices_list, mac)))
        goto Exit;

    ret = _ble_disconnect(dev);

Exit:
    xSemaphoreGiveRecursive(devices_list_semaphore);
    return ret;
}

int ble_disconnect_all(void)
{
    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    ble_device_foreach(devices_list, _ble_disconnect);

    xSemaphoreGiveRecursive(devices_list_semaphore);
    return 0;
}

int ble_services_scan(mac_addr_t mac)
{
    ble_device_t *dev;
    int ret;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    if (!(dev = ble_device_find_by_mac(devices_list, mac)))
    {
        xSemaphoreGiveRecursive(devices_list_semaphore);
        return -1;
    }

    ret = esp_ble_gattc_search_service(g_gattc_if, dev->conn_id, NULL);

    xSemaphoreGiveRecursive(devices_list_semaphore);
    return ret;
}

static void esp_uuid_to_bt_uuid(esp_bt_uuid_t esp, ble_uuid_t bt)
{
    /* Bluetooth SIG Base UUID */
    static uint8_t base_uuid[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00,
                                    0x80, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    if (esp.len == ESP_UUID_LEN_128)
        memcpy(bt, &esp.uuid, esp.len);
    else
    {
        memcpy(bt, base_uuid, sizeof(base_uuid));
        memcpy(&bt[12], &esp.uuid, esp.len);
    }
}

static void ble_update_cache(ble_device_t *dev)
{
    esp_gattc_db_elem_t *db;
    ble_service_t *service = NULL;
    ble_characteristic_t *characteristic = NULL;
    ble_uuid_t service_uuid, characteristic_uuid;
    uint16_t count, i;

    if (!dev)
        return;

    /* Get GATT DB Size */
    if (esp_ble_gattc_get_attr_count(g_gattc_if, dev->conn_id, ESP_GATT_DB_ALL,
                                     0, UINT16_MAX, INVALID_HANDLE, &count))
    {
        return;
    }

    /* Get GATT DB */
    db = malloc(sizeof(*db) * count);
    if (esp_ble_gattc_get_db(g_gattc_if, dev->conn_id, 0, UINT16_MAX, db,
                             &count))
    {
        free(db);
        return;
    }

    /* Find all characteristics and cache them */
    for (i = 0; i < count; i++)
    {
        if (db[i].type == ESP_GATT_DB_PRIMARY_SERVICE)
        {
            esp_uuid_to_bt_uuid(db[i].uuid, service_uuid);
            service = ble_device_service_add(dev, service_uuid);
        }
        else if (db[i].type == ESP_GATT_DB_CHARACTERISTIC)
        {
            esp_uuid_to_bt_uuid(db[i].uuid, characteristic_uuid);

            uint8_t index = 0;

            for (ble_characteristic_t *cur = service->characteristics;
                 cur != NULL; cur = cur->next)
            {
                if (ble_uuid_equal(cur->uuid, characteristic_uuid))
                    index++;
            }

            characteristic = ble_device_characteristic_add(service,
                                                           characteristic_uuid, index, db[i].attribute_handle,
                                                           db[i].properties);
        }
        else if (db[i].type == ESP_GATT_DB_DESCRIPTOR &&
                 db[i].uuid.len == ESP_UUID_LEN_16 &&
                 db[i].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG)
        {
            characteristic->client_config_handle = db[i].attribute_handle;
        }
    }
    free(db);
}

int ble_foreach_characteristic(mac_addr_t mac,
                               ble_on_device_characteristic_found_cb_t cb)
{
    ble_device_t *dev;
    ble_service_t *service;
    ble_characteristic_t *characteristic;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    if (!(dev = ble_device_find_by_mac(devices_list, mac)))
    {
        xSemaphoreGiveRecursive(devices_list_semaphore);
        return -1;
    }

    /* If services list is empty, try to update it */
    if (!dev->services)
        ble_update_cache(dev);

    for (service = dev->services; service; service = service->next)
    {
        for (characteristic = service->characteristics; characteristic;
             characteristic = characteristic->next)
        {
            cb(mac, service->uuid, characteristic->uuid,
               characteristic->index, characteristic->properties);
        }
    }

    xSemaphoreGiveRecursive(devices_list_semaphore);
    return 0;
}

int ble_characteristic_read(mac_addr_t mac, ble_uuid_t service_uuid,
                            ble_uuid_t characteristic_uuid, uint8_t index)
{
    ble_device_t *device;
    ble_service_t *service;
    ble_characteristic_t *characteristic;
    int ret = -1;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    if (!(device = ble_device_find_by_mac(devices_list, mac)))
        goto Exit;

    if (!(service = ble_device_service_find(device, service_uuid)))
        goto Exit;

    if (!(characteristic = ble_device_characteristic_find_by_uuid(service,
                                                                  characteristic_uuid, index)))
    {
        goto Exit;
    }

    if (!(characteristic->properties & CHAR_PROP_READ))
        goto Exit;

    ble_operation_enqueue(&operation_queue, BLE_OPERATION_TYPE_READ, device,
                          characteristic, 0, NULL);

    ret = 0;
Exit:
    xSemaphoreGiveRecursive(devices_list_semaphore);
    return ret;
}

int ble_characteristic_write(mac_addr_t mac, ble_uuid_t service_uuid,
                             ble_uuid_t characteristic_uuid, uint8_t index, const uint8_t *value,
                             size_t value_len)
{
    ble_device_t *device;
    ble_service_t *service;
    ble_characteristic_t *characteristic;
    int ret = -1;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    if (!(device = ble_device_find_by_mac(devices_list, mac)))
        goto Exit;

    if (!(service = ble_device_service_find(device, service_uuid)))
        goto Exit;

    if (!(characteristic = ble_device_characteristic_find_by_uuid(service,
                                                                  characteristic_uuid, index)))
    {
        goto Exit;
    }

    if (!(characteristic->properties & (CHAR_PROP_WRITE | CHAR_PROP_WRITE_NR)))
        goto Exit;

    ble_operation_enqueue(&operation_queue,
                          characteristic->properties & CHAR_PROP_WRITE ? BLE_OPERATION_TYPE_WRITE : BLE_OPERATION_TYPE_WRITE_NR, device,
                          characteristic, value_len, value);

    ret = 0;
Exit:
    xSemaphoreGiveRecursive(devices_list_semaphore);
    return ret;
}

int ble_characteristic_notify_register(mac_addr_t mac, ble_uuid_t service_uuid,
                                       ble_uuid_t characteristic_uuid, uint8_t index)
{
    uint16_t enable = htole16(0x1);
    ble_device_t *device;
    ble_service_t *service;
    ble_characteristic_t *characteristic;
    int ret = -1;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    if (!(device = ble_device_find_by_mac(devices_list, mac)))
        goto Exit;

    if (!(service = ble_device_service_find(device, service_uuid)))
        goto Exit;

    if (!(characteristic = ble_device_characteristic_find_by_uuid(service,
                                                                  characteristic_uuid, index)))
    {
        goto Exit;
    }

    if (!(characteristic->properties & (CHAR_PROP_NOTIFY | CHAR_PROP_INDICATE)))
        goto Exit;

    if (characteristic->client_config_handle == 0)
        goto Exit;

    if (characteristic->properties & CHAR_PROP_INDICATE)
        enable = htole16(0x2);

    if (esp_ble_gattc_register_for_notify(g_gattc_if, device->mac,
                                          characteristic->handle))
    {
        ESP_LOGE(TAG, "Failed registring for notification for char " UUID_FMT,
                 UUID_PARAM(characteristic_uuid));
        goto Exit;
    }

    ble_operation_enqueue(&operation_queue, BLE_OPERATION_TYPE_WRITE_CHAR,
                          device, characteristic, sizeof(enable), (uint8_t *)&enable);

    ret = 0;
Exit:
    xSemaphoreGiveRecursive(devices_list_semaphore);
    return ret;
}

int ble_characteristic_notify_unregister(mac_addr_t mac,
                                         ble_uuid_t service_uuid, ble_uuid_t characteristic_uuid, uint8_t index)
{
    ble_device_t *device;
    ble_service_t *service;
    ble_characteristic_t *characteristic;
    int ret = -1;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    if (!(device = ble_device_find_by_mac(devices_list, mac)))
        goto Exit;

    if (!(service = ble_device_service_find(device, service_uuid)))
        goto Exit;

    if (!(characteristic = ble_device_characteristic_find_by_uuid(service,
                                                                  characteristic_uuid, index)))
    {
        goto Exit;
    }

    ret = esp_ble_gattc_unregister_for_notify(g_gattc_if, device->mac,
                                              characteristic->handle);

Exit:
    xSemaphoreGiveRecursive(devices_list_semaphore);
    return ret;
}

/* Management */
ble_dev_t *ble_devices_list_get(size_t *number_of_devices)
{
    ble_device_t *iter;
    ble_dev_t *devices;
    size_t i, count = 0;

    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

    for (iter = devices_list; iter; iter = iter->next)
        count++;

    devices = malloc(sizeof(*devices) * count);
    for (iter = devices_list, i = 0; iter; iter = iter->next, i++)
    {
        strncpy(devices[i].name, iter->name ?: "", 32);
        memcpy(devices[i].mac, iter->mac, sizeof(mac_addr_t));
        devices[i].connected = iter->conn_id != 0xffff;
    }

    xSemaphoreGiveRecursive(devices_list_semaphore);

    *number_of_devices = count;
    return devices;
}

void ble_devices_list_free(ble_dev_t *devices)
{
    free(devices);
}

static char *ble_device_name_get(uint8_t *adv_data)
{
    static char name[32];
    uint8_t len;
    uint8_t *data;

    if (!(data = esp_ble_resolve_adv_data(adv_data, ESP_BLE_AD_TYPE_NAME_CMPL,
                                          &len)))
    {
        if (!(data = esp_ble_resolve_adv_data(adv_data,
                                              ESP_BLE_AD_TYPE_NAME_SHORT, &len)))
        {
            return NULL;
        }
    }

    strncpy(name, (char *)data, len);
    name[len] = '\0';
    return name;
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    ESP_LOGD(TAG, "Received GAP event %d (%s)", event, gap_event_to_str(event));

    switch (event)
    {
    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
        if (param->local_privacy_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(TAG, "Configuring local privacy failed, status: 0x%x",
                     param->local_privacy_cmpl.status);
            break;
        }
        ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&ble_scan_params));
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(TAG, "Scanning starting failed, status: 0x%x",
                     param->scan_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(TAG, "Scanning stopping failed, status: 0x%x",
                     param->scan_stop_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
    {
        ble_device_t *device;
        char *name;

        /* If scan was stopped before this device was found, ignore it */
        if (!scan_requested)
            break;

        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT)
            break;

        /* Check if this device is a broadcaster */
        broadcaster_ops_t *broadcaster_ops = broadcaster_ops_get(
            param->scan_rst.ble_adv, param->scan_rst.adv_data_len);
        if (broadcaster_ops && on_broadcaster_discovered_cb)
        {
            on_broadcaster_discovered_cb(param->scan_rst.bda,
                                         param->scan_rst.ble_adv, param->scan_rst.adv_data_len,
                                         param->scan_rst.rssi, broadcaster_ops);
        }

        xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

        name = ble_device_name_get(param->scan_rst.ble_adv);
        device = ble_device_find_by_mac(devices_list, param->scan_rst.bda);

        /* Device already discovered */
        if (device)
        {

            /* Update name, if none was available */
            if (!device->name && name)
            {
                ble_device_update_name(device, name);

                // /* Notify app only on newly connected devices */
                // if (on_device_discovered_cb && name)
                //     on_device_discovered_cb(param->scan_rst.bda, name, param->scan_rst.rssi);
            }
            xSemaphoreGiveRecursive(devices_list_semaphore);
            break;
        }
        /* Cache device information */
        if (name)
        {
            name = ble_device_name_get(param->scan_rst.ble_adv);

            ble_device_add(&devices_list, name, param->scan_rst.bda,
                           param->scan_rst.ble_addr_type, 0xffff);
        }
        xSemaphoreGiveRecursive(devices_list_semaphore);

        /* Notify app only on newly connected devices */
        if (on_device_discovered_cb && name)
        {
            ESP_LOGE("device name :", "%s ", name);

            on_device_discovered_cb(param->scan_rst.bda, name, param->scan_rst.rssi);
        }
        // on_device_discovered_cb(param->scan_rst.bda, param->scan_rst.rssi);

        break;
    }
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true,
                              on_passkey_requested_cb ? on_passkey_requested_cb(param->ble_security.ble_req.bd_addr) : 0);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
    {
        ble_device_t *device;

        xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

        if ((device = ble_device_find_by_mac(devices_list,
                                             param->ble_security.auth_cmpl.bd_addr)))
        {
            device->is_authenticating = 0;
        }

        xSemaphoreGiveRecursive(devices_list_semaphore);

        if (!param->ble_security.auth_cmpl.success)
        {
            ESP_LOGE(TAG, "Authentication failed, status: 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    }
#ifdef CONFIG_SET_RAW_ADV_DATA
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done == 0)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#else
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~adv_config_flag);
        if (adv_config_done == 0)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
#endif
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        // advertising start complete event to indicate advertising start successfully or failed
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTS_TAG, "Advertising start failed");
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTS_TAG, "Advertising stop failed");
        }
        else
        {
            ESP_LOGI(GATTS_TAG, "Stop adv successfully");
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(GATTS_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                 param->update_conn_params.status,
                 param->update_conn_params.min_int,
                 param->update_conn_params.max_int,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;
    default:
        ESP_LOGD(TAG, "GAP event %d wasn't handled", event);
        break;
    }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param)
{
    uint8_t need_dequeue = 0;

    ESP_LOGD(TAG, "Received GATTC event %d (%s), gattc_if %d", event,
             gattc_event_to_str(event), gattc_if);

    switch (event)
    {
    case ESP_GATTC_REG_EVT:
        /* Save the interface for later */
        g_gattc_if = gattc_if;
        break;
    case ESP_GATTC_OPEN_EVT:
    {
        ble_device_t *device;

        need_dequeue = 1;

        /* Resume scanning, if requested */
        if (scan_requested)
            esp_ble_gap_start_scanning(-1);

        if (param->open.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Open failed, status = 0x%x", param->open.status);
            /* Remove device from cache */
            xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);
            ble_device_remove_by_mac(&devices_list, param->open.remote_bda);
            xSemaphoreGiveRecursive(devices_list_semaphore);
            break;
        }

        /* Save device connection ID */
        xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);
        device = ble_device_find_by_mac(devices_list, param->open.remote_bda);
        device->conn_id = param->open.conn_id;
        xSemaphoreGiveRecursive(devices_list_semaphore);

        /* Configure MTU */
        ESP_ERROR_CHECK(esp_ble_gattc_send_mtu_req(gattc_if,
                                                   param->open.conn_id));

        break;
    }
    case ESP_GATTC_CLOSE_EVT:
        ESP_LOGI(TAG, "Connection closed, reason = 0x%x", param->close.reason);
        /* Notify app that the device is disconnected */
        if (on_device_disconnected_cb)
            on_device_disconnected_cb(param->close.remote_bda);

        /* Remove queued requests for this device */
        ble_operation_remove_by_mac(&operation_queue, param->close.remote_bda);

        /* Remove device from cache */
        xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);
        ble_device_remove_by_mac(&devices_list, param->close.remote_bda);
        xSemaphoreGiveRecursive(devices_list_semaphore);
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Configuring MTU failed, status = 0x%x",
                     param->cfg_mtu.status);
        }

        /* Notify app that the device is connected */
        if (on_device_connected_cb)
        {
            ble_device_t *dev;

            xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

            if ((dev = ble_device_find_by_conn_id(devices_list,
                                                  param->cfg_mtu.conn_id)))
            {
                on_device_connected_cb(dev->mac);
            }

            xSemaphoreGiveRecursive(devices_list_semaphore);
        }

        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Searching services failed, status = 0x%x",
                     param->search_cmpl.status);
            break;
        }

        /* Notify app that the services were discovered */
        if (on_device_services_discovered_cb)
        {
            ble_device_t *dev;

            xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

            if ((dev = ble_device_find_by_conn_id(devices_list,
                                                  param->search_cmpl.conn_id)))
            {
                on_device_services_discovered_cb(dev->mac);
            }

            xSemaphoreGiveRecursive(devices_list_semaphore);
        }

        break;
    case ESP_GATTC_READ_CHAR_EVT:
    {
        ble_device_t *device;
        ble_service_t *service;
        ble_characteristic_t *characteristic;

        need_dequeue = 1;

        xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

        if (!(device = ble_device_find_by_conn_id(devices_list,
                                                  param->read.conn_id)))
        {
            xSemaphoreGiveRecursive(devices_list_semaphore);
            break;
        }

        if (param->read.status != ESP_GATT_OK)
        {
            /* Check if authentication/encryption is needed */
            if (param->read.status == ESP_GATT_INSUF_AUTHENTICATION ||
                param->read.status == ESP_GATT_INSUF_ENCRYPTION)
            {
                if (!device->is_authenticating)
                {
                    device->is_authenticating = 1;
                    esp_ble_set_encryption(device->mac,
                                           ESP_BLE_SEC_ENCRYPT_MITM);
                }
                /* Try again */
                esp_ble_gattc_read_char(g_gattc_if, param->read.conn_id,
                                        param->read.handle, ESP_GATT_AUTH_REQ_NONE);
                need_dequeue = 0;
            }
            else
            {
                ESP_LOGE(TAG, "Failed reading characteristic, status = 0x%x",
                         param->read.status);
            }
        }
        else if (!ble_device_info_get_by_conn_id_handle(devices_list,
                                                        param->read.conn_id, param->read.handle, &device, &service,
                                                        &characteristic) &&
                 on_device_characteristic_value_cb)
        {
            on_device_characteristic_value_cb(device->mac, service->uuid,
                                              characteristic->uuid, characteristic->index,
                                              param->read.value, param->read.value_len);
        }

        xSemaphoreGiveRecursive(devices_list_semaphore);
        break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT:
        need_dequeue = 1;
        if (param->write.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Failed writing characteristic, status = 0x%x",
                     param->write.status);
        }
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG,
                     "Failed writing characteristic descriptor, status = 0x%x",
                     param->write.status);
        }
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status != ESP_GATT_OK)
        {
            ESP_LOGE(TAG, "Failed registering for notification, status = 0x%x",
                     param->reg_for_notify.status);
        }
        break;
    case ESP_GATTC_NOTIFY_EVT:
    {
        ble_device_t *device;
        ble_service_t *service;
        ble_characteristic_t *characteristic;

        xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);

        if (!ble_device_info_get_by_conn_id_handle(devices_list,
                                                   param->notify.conn_id, param->notify.handle, &device, &service,
                                                   &characteristic) &&
            on_device_characteristic_value_cb)
        {
            on_device_characteristic_value_cb(device->mac, service->uuid,
                                              characteristic->uuid, characteristic->index, param->notify.value,
                                              param->notify.value_len);
        }

        xSemaphoreGiveRecursive(devices_list_semaphore);
        break;
    }
    default:
        ESP_LOGD(TAG, "GATTC event %d wasn't handled", event);
        break;
    }

    if (need_dequeue)
        ble_operation_dequeue(&operation_queue);
}

static void ble_purge_device_list_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGD(TAG, "Purging non-connected devices");
    xSemaphoreTakeRecursive(devices_list_semaphore, portMAX_DELAY);
    ble_device_remove_disconnected(&devices_list);
    xSemaphoreGiveRecursive(devices_list_semaphore);
}

int ble_initialize(void)
{
    ESP_LOGD(TAG, "Initializing BLE client");
    // ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ESP_LOGE(GATTS_TAG, "%d", bt_cfg.ble_max_act);
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT,
                                         ESP_PWR_LVL_P9));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_ble_gap_config_local_privacy(true));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_gattc_cb));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));

    // ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    //  ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_A_APP_ID));
    //  ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_B_APP_ID));

    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(500));
    /* Set security IO capability to KeyboardOnly */
    esp_ble_io_cap_t iocap = ESP_IO_CAP_IN;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap,
                                   sizeof(iocap));

    if (!(purge_device_list_timer = xTimerCreate("purge_device_list_timer",
                                                 pdMS_TO_TICKS(60 * 60 * 1000), pdTRUE, NULL,
                                                 ble_purge_device_list_timer_cb)))
    {
        return -1;
    }

    if (!(devices_list_semaphore = xSemaphoreCreateRecursiveMutex()))
        return -1;

    xTimerStart(purge_device_list_timer, 0);

    return 0;
}