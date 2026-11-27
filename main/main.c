#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_rom_sys.h"

static const char *TAG = "home_cam";

// =========================================================================
// ĐỊNH NGHĨA CHÂN GPIO (HARDWARE PIN MAP - THEO SƠ ĐỒ THỰC TẾ)
// =========================================================================
// 1. Chân điều khiển Relay Output & Còi (Outputs)
#define PIN_DOOR        21  // GPIO 21: RELAY2 (cửa) - Thay thế GPIO 21 (Chân 27 của U1)
#define PIN_FAN         20  // GPIO 20: RELAY3 (quạt)
#define PIN_LAMP        47  // GPIO 47: RELAY1 (đèn)
#define PIN_BUZZER      48  // GPIO 48: Còi + LED tích hợp ESP32-S3-CAM

// 2. Chân cảm biến đầu vào (Inputs)
#define PIN_PIR         14  // GPIO 14: PIR (Digital - Cảm biến chuyển động)
#define PIN_GAS         2   // GPIO 2:  Cảm biến khí gas (Digital - ngõ ra DO)
#define PIN_RAIN        1   // GPIO 1:  Cảm biến mưa (Digital)
#define PIN_DHT         19  // GPIO 19: Cảm biến nhiệt độ & độ ẩm DHT

// 3. Chân giao tiếp SPI với Module RFID RC522
#define PIN_RFID_MISO   39  // GPIO 39: MISO
#define PIN_RFID_MOSI   40  // GPIO 40: MOSI
#define PIN_RFID_SCLK   41  // GPIO 41: SCK
#define PIN_RFID_CS     42  // GPIO 42: SDA (CS - Chip Select)
#define PIN_RFID_RST    38  // GPIO 38: RST-rfid (Reset cứng)

// =========================================================================
// BIẾN TOÀN CỤC & ĐỒNG BỘ TRẠNG THÁI
// =========================================================================
#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group = NULL;

static spi_device_handle_t rfid_spi_handle = NULL;

#define DEFAULT_SERVER_URL "https://esp32.banyan.io.vn"
static char g_server_url[128] = DEFAULT_SERVER_URL;

// Trạng thái các cảm biến
static volatile bool g_pir_detected = false;
static volatile bool g_gas_detected = false;
static volatile bool g_rain_detected = false;
static volatile bool g_sensor_3 = false; // Phản chiếu trạng thái rain/door
static volatile float g_temperature = 28.5f;
static volatile float g_humidity = 65.0f;

// Trạng thái thiết bị ngoại vi & điều khiển
static volatile bool g_fan_on = false;
static volatile bool g_door_unlocked = false;
static volatile int64_t g_door_unlock_until = 0;
static volatile bool g_lamp_on = false;
static volatile bool g_buzzer_on = false;
static volatile bool g_buzzer_manual_override = false;
static volatile bool g_rfid_enroll_mode = false;

static int resp_len = 0;

// =========================================================================
// KHAI BÁO NGUYÊN MẪU HÀM (PROTOTYPES)
// =========================================================================
static void init_pins(void);
static void init_rfid(void);
static void init_camera(void);
static void init_wifi(void);
static void load_server_config(void);

static void control_task(void *pvParameters);
static void rfid_task(void *pvParameters);
static void relay_discovery_task(void *pvParameters);
static void server_sync_task(void *pvParameters);
static void camera_stream_task(void *pvParameters);

static void apply_relay_outputs(void);
static void read_sensors(void);
static inline void set_pin_state(gpio_num_t pin, uint32_t level);

static void rc522_write_reg(uint8_t reg, uint8_t val);
static uint8_t rc522_read_reg(uint8_t reg);
static bool rc522_transceive(const uint8_t *send_data, int send_len, uint8_t *recv_data, int *recv_len);
static bool rc522_read_uid(uint8_t *uid_out);
static bool is_rfid_uid_authorized(const uint8_t *uid);
static void save_rfid_uid_to_nvs(const uint8_t *uid);
static void register_rfid_uid_to_server(const uint8_t *uid);
static void save_server_url_to_nvs(const char *url);

static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static esp_err_t rfid_http_event_handler(esp_http_client_event_t *evt);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

static esp_err_t http_event_handler(esp_http_client_event_t *evt);
static esp_err_t rfid_http_event_handler(esp_http_client_event_t *evt);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// =========================================================================
// 1. KHỞI TẠO CHÂN GPIO (FUN_42012b4c -> init_pins)
// =========================================================================
static inline void set_pin_state(gpio_num_t pin, uint32_t level)
{
    gpio_set_level(pin, level);
}

static void apply_relay_outputs(void)
{
    set_pin_state(PIN_DOOR, g_door_unlocked ? 1 : 0);
    set_pin_state(PIN_FAN, g_fan_on ? 1 : 0);
    set_pin_state(PIN_LAMP, g_lamp_on ? 1 : 0);
    set_pin_state(PIN_BUZZER, g_buzzer_on ? 1 : 0);
}

static void init_pins(void)
{
    // Reset các chân output về trạng thái sạch (ngắt mọi chức năng RTC/ADC phụ)
    gpio_reset_pin(PIN_DOOR);
    gpio_reset_pin(PIN_FAN);
    gpio_reset_pin(PIN_LAMP);
    gpio_reset_pin(PIN_BUZZER);

    // Cấu hình các chân Output (Relay & Buzzer) chế độ Push-Pull thuần túy
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_DOOR) | (1ULL << PIN_FAN) | (1ULL << PIN_LAMP) | (1ULL << PIN_BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));

    // Nâng khả năng phát dòng lên mức tối đa (GPIO_DRIVE_CAP_3 ~40mA) để không bị sụt áp
    gpio_set_drive_capability(PIN_DOOR, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_FAN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_LAMP, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(PIN_BUZZER, GPIO_DRIVE_CAP_3);

    // Cấu hình các chân Input (PIR, Gas, Rain, DHT)
    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << PIN_PIR) | (1ULL << PIN_GAS) | (1ULL << PIN_RAIN) | (1ULL << PIN_DHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&inputs));

    // Đặt mức logic ban đầu: tắt toàn bộ thiết bị (mức 0V)
    set_pin_state(PIN_BUZZER, 0); // GPIO 48
    set_pin_state(PIN_DOOR, 0);   // GPIO 21 (RELAY2)
    set_pin_state(PIN_FAN, 0);    // GPIO 20 (RELAY3)
    set_pin_state(PIN_LAMP, 0);   // GPIO 47 (RELAY1)

    ESP_LOGI(TAG, "GPIO initialized successfully (Max drive strength 40mA enabled)");
}

// =========================================================================
// ĐỌC CẢM BIẾN (FUN_42012a64 -> read_sensors)
// =========================================================================
static bool read_dht_sensor(float *temp, float *hum)
{
    uint8_t data[5] = {0};
    gpio_set_direction(PIN_DHT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DHT, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); // Kéo LOW ít nhất 18ms
    gpio_set_level(PIN_DHT, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(PIN_DHT, GPIO_MODE_INPUT);

    int timeout = 100;
    while (gpio_get_level(PIN_DHT) == 1) {
        if (--timeout == 0) return false;
        esp_rom_delay_us(1);
    }
    timeout = 100;
    while (gpio_get_level(PIN_DHT) == 0) {
        if (--timeout == 0) return false;
        esp_rom_delay_us(1);
    }
    timeout = 100;
    while (gpio_get_level(PIN_DHT) == 1) {
        if (--timeout == 0) return false;
        esp_rom_delay_us(1);
    }

    for (int i = 0; i < 40; i++) {
        timeout = 100;
        while (gpio_get_level(PIN_DHT) == 0) {
            if (--timeout == 0) return false;
            esp_rom_delay_us(1);
        }
        int count = 0;
        while (gpio_get_level(PIN_DHT) == 1) {
            count++;
            if (count > 100) break;
            esp_rom_delay_us(1);
        }
        data[i / 8] <<= 1;
        if (count > 35) {
            data[i / 8] |= 1;
        }
    }

    if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        return false;
    }

    float h = (float)data[0] + (float)data[1] * 0.1f;
    float t = (float)data[2] + (float)data[3] * 0.1f;
    if (h > 0.0f && h <= 100.0f) {
        *hum = h;
        *temp = t;
        return true;
    }
    return false;
}

static void read_sensors(void)
{
    // PIR sensor: mức HIGH khi phát hiện chuyển động
    g_pir_detected = (gpio_get_level(PIN_PIR) == 1);

    // MQ-2 sensor: ngõ ra DO thường mức LOW khi nồng độ gas/khói vượt ngưỡng
    g_gas_detected = (gpio_get_level(PIN_GAS) == 0);

    // Cảm biến mưa / cửa từ: mức LOW khi chạm nước / cửa đóng
    g_rain_detected = (gpio_get_level(PIN_RAIN) == 0);
    g_sensor_3 = g_rain_detected;

    // Đọc cảm biến nhiệt độ & độ ẩm
    float t = g_temperature, h = g_humidity;
    if (read_dht_sensor(&t, &h)) {
        g_temperature = t;
        g_humidity = h;
    }
}

// =========================================================================
// 2. KHỞI TẠO & GIAO TIẾP RFID MFRC522 (FUN_42012bc0 -> init_rfid)
// =========================================================================
static void rc522_write_reg(uint8_t reg, uint8_t val)
{
    if (rfid_spi_handle == NULL) return;
    uint8_t tx_buf[2] = { (uint8_t)((reg << 1) & 0x7E), val };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_buf,
    };
    spi_device_transmit(rfid_spi_handle, &t);
}

static uint8_t rc522_read_reg(uint8_t reg)
{
    if (rfid_spi_handle == NULL) return 0;
    uint8_t tx_buf[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0x00 };
    uint8_t rx_buf[2] = { 0 };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    spi_device_transmit(rfid_spi_handle, &t);
    return rx_buf[1];
}

static bool rc522_transceive(const uint8_t *send_data, int send_len, uint8_t *recv_data, int *recv_len)
{
    if (rfid_spi_handle == NULL || send_data == NULL || recv_data == NULL || recv_len == NULL) {
        return false;
    }

    rc522_write_reg(0x01, 0x00); // CommandReg = Idle
    rc522_write_reg(0x04, 0x7F); // Xóa cờ ngắt ComIrqReg
    rc522_write_reg(0x0A, 0x80); // FlushBuffer FIFO

    for (int i = 0; i < send_len; i++) {
        rc522_write_reg(0x09, send_data[i]); // FIFODataReg
    }

    rc522_write_reg(0x01, 0x0C); // CommandReg = Transceive

    uint8_t bit_framing = rc522_read_reg(0x0D);
    rc522_write_reg(0x0D, bit_framing | 0x80); // StartSend

    int wait_loops = 250;
    while (wait_loops > 0) {
        uint8_t irq_val = rc522_read_reg(0x04);
        if (irq_val & 0x30) { // RxIRq hoặc IdleIRq
            break;
        }
        esp_rom_delay_us(100);
        wait_loops--;
    }

    rc522_write_reg(0x0D, bit_framing & (~0x80));

    if (wait_loops == 0) {
        return false;
    }

    uint8_t error_reg = rc522_read_reg(0x06);
    if (error_reg & 0x1B) {
        return false;
    }

    uint8_t bytes_in_fifo = rc522_read_reg(0x0A) & 0x7F;
    int max_to_read = (*recv_len < bytes_in_fifo) ? *recv_len : bytes_in_fifo;

    for (int i = 0; i < max_to_read; i++) {
        recv_data[i] = rc522_read_reg(0x09);
    }
    *recv_len = max_to_read;

    return (max_to_read > 0);
}

static bool rc522_read_uid(uint8_t *uid_out)
{
    uint8_t cmd_reqa = 0x26; // PICC_CMD_REQA
    uint8_t atqa[2] = {0};
    int rx_len = 2;

    rc522_write_reg(0x0D, 0x07); // BitFramingReg: truyền 7 bit cho lệnh REQA
    if (!rc522_transceive(&cmd_reqa, 1, atqa, &rx_len) || rx_len != 2) {
        return false;
    }

    uint8_t cmd_anticoll[2] = {0x93, 0x20}; // PICC_CMD_SEL_CL1
    uint8_t raw_uid[5] = {0};
    rx_len = 5;

    rc522_write_reg(0x0D, 0x00); // BitFramingReg: truyền đủ 8 bit
    if (!rc522_transceive(cmd_anticoll, 2, raw_uid, &rx_len) || rx_len != 5) {
        return false;
    }

    // Kiểm tra checksum XOR của 4 byte UID
    if ((raw_uid[0] ^ raw_uid[1] ^ raw_uid[2] ^ raw_uid[3]) != raw_uid[4]) {
        return false;
    }

    memcpy(uid_out, raw_uid, 4);
    return true;
}

static void init_rfid(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_RFID_MOSI, // GPIO 40
        .miso_io_num = PIN_RFID_MISO, // GPIO 39
        .sclk_io_num = PIN_RFID_SCLK, // GPIO 41
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,    // 1 MHz
        .mode = 0,
        .spics_io_num = PIN_RFID_CS,  // GPIO 42
        .queue_size = 1,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &rfid_spi_handle));

    // Reset module RC522 qua chân RST = GPIO 38
    gpio_set_direction(PIN_RFID_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RFID_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RFID_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Cấu hình các thanh ghi MFRC522
    rc522_write_reg(0x01, 0x0F); // SoftReset
    vTaskDelay(pdMS_TO_TICKS(50));
    rc522_write_reg(0x2A, 0x8D); // TModeReg
    rc522_write_reg(0x2B, 0x3E); // TPrescalerReg
    rc522_write_reg(0x2D, 0x1E); // TReloadRegL
    rc522_write_reg(0x2C, 0x00); // TReloadRegH
    rc522_write_reg(0x15, 0x40); // TxASKReg
    rc522_write_reg(0x11, 0x3D); // ModeReg

    // Bật ăng-ten
    uint8_t tx_ctrl = rc522_read_reg(0x14);
    rc522_write_reg(0x14, tx_ctrl | 0x03);

    uint8_t ver = rc522_read_reg(0x37);
    if (ver == 0x91 || ver == 0x92) {
        ESP_LOGI(TAG, "RC522 detected, Version: 0x%02x", ver);
    } else {
        ESP_LOGW(TAG, "RC522 not detected, Version: 0x%02x", ver);
    }
    ESP_LOGI(TAG, "RFID SPI ready (RST=%d, CS=%d)", PIN_RFID_RST, PIN_RFID_CS);
}

// =========================================================================
// 3. KHỞI TẠO CAMERA OV2640 (FUN_42012d1c -> init_camera)
// =========================================================================
static void init_camera(void)
{
    bool has_psram = (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0);
    ESP_LOGI(TAG, "Initializing camera (PSRAM detected: %s)...", has_psram ? "YES" : "NO");

    camera_config_t config = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 15,
        .pin_sccb_sda = 4,
        .pin_sccb_scl = 5,
        .pin_d7 = 16,
        .pin_d6 = 17,
        .pin_d5 = 18,
        .pin_d4 = 12,
        .pin_d3 = 10,
        .pin_d2 = 8,
        .pin_d1 = 9,
        .pin_d0 = 11,
        .pin_vsync = 6,
        .pin_href = 7,
        .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = has_psram ? 2 : 1,
        .fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = has_psram ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK && config.fb_location == CAMERA_FB_IN_PSRAM) {
        ESP_LOGW(TAG, "PSRAM fb allocation failed (0x%x), retrying in internal DRAM with fb_count=1...", err);
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count = 1;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        err = esp_camera_init(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x (%s)", err, esp_err_to_name(err));
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_vflip(s, 0);
        ESP_LOGI(TAG, "OV2640 camera ready (PID: 0x%x)", s->id.PID);
    } else {
        ESP_LOGI(TAG, "OV2640 camera ready");
    }
}

// =========================================================================
// 4. KHỞI TẠO WI-FI & SỰ KIỆN MẠNG (FUN_42012dc8 -> init_wifi)
// =========================================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void init_wifi(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "AIThings",
            .password = "12345678",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "Wi-Fi initialization started for SSID: %s", wifi_config.sta.ssid);
}

// =========================================================================
// 5. CẤU HÌNH SERVER URL VÀ NVS (FUN_42012fe4 -> load_server_config)
// =========================================================================
static void sanitize_server_url(char *url)
{
    size_t len = strlen(url);
    while (len > 0 && (url[len - 1] == '/' || url[len - 1] == ' ' || url[len - 1] == '\r' || url[len - 1] == '\n')) {
        url[len - 1] = '\0';
        len--;
    }
}

static void save_server_url_to_nvs(const char *url)
{
    char clean_url[128];
    strncpy(clean_url, url, sizeof(clean_url) - 1);
    clean_url[sizeof(clean_url) - 1] = '\0';
    sanitize_server_url(clean_url);

    nvs_handle_t nvs_handle;
    if (nvs_open("netcfg", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, "server_url", clean_url);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

static void load_server_config(void)
{
    // Luôn ưu tiên DEFAULT_SERVER_URL được cấu hình trong mã nguồn
    strncpy(g_server_url, DEFAULT_SERVER_URL, sizeof(g_server_url) - 1);
    g_server_url[sizeof(g_server_url) - 1] = '\0';
    sanitize_server_url(g_server_url);
    save_server_url_to_nvs(g_server_url);

    ESP_LOGI(TAG, "Configured Server URL: %s", g_server_url);
}

// =========================================================================
// TASK 1: control_task (FUN_42012aa0) - Giám sát an toàn & tự động hóa
// =========================================================================
static void control_task(void *pvParameters)
{
    while (true) {
        read_sensors();

        // 1. Tự động khóa cửa sau thời gian mở (20 giây)
        if (g_door_unlock_until > 0 && esp_timer_get_time() >= g_door_unlock_until) {
            g_door_unlocked = false;
            g_door_unlock_until = 0;
            apply_relay_outputs();
            ESP_LOGI(TAG, "Door locked automatically after timeout");
        }

        // 2. Cảnh báo rò rỉ khí gas: kích hoạt quạt và còi hú
        if (g_gas_detected) {
            g_fan_on = true;
            g_buzzer_on = true;
        } else if (!g_buzzer_manual_override) {
            g_buzzer_on = false;
        }

        // 3. Tự động bật quạt nếu nhiệt độ vượt ngưỡng 40.0°C
        if (g_temperature > 40.0f) {
            g_fan_on = true;
        }

        apply_relay_outputs();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// =========================================================================
// QUẢN LÝ THẺ RFID & GIAO TIẾP VỚI SERVER
// =========================================================================
static esp_err_t rfid_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (!esp_http_client_is_chunked_response(evt->client) && evt->user_data) {
            char *buf = (char *)evt->user_data;
            int current_len = strlen(buf);
            int copy_len = evt->data_len;
            if (current_len + copy_len < 163) {
                memcpy(buf + current_len, evt->data, copy_len);
                buf[current_len + copy_len] = '\0';
            }
        }
    }
    return ESP_OK;
}

static bool is_rfid_uid_authorized(const uint8_t *uid)
{
    char url[192];
    char post_data[128];
    char resp_buf[164] = {0};
    bool authorized = false;

    snprintf(url, sizeof(url), "%s/api/device/rfid/check", g_server_url);
    snprintf(post_data, sizeof(post_data),
             "{\"device_id\":\"%s\",\"uid\":\"%02X %02X %02X %02X\"}",
             "esp32s3cam", uid[0], uid[1], uid[2], uid[3]);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
        .event_handler = rfid_http_event_handler,
        .user_data = resp_buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);

        if (err == ESP_OK && status == 200 && strstr(resp_buf, "\"authorized\":true") != NULL) {
            authorized = true;
        }
        esp_http_client_cleanup(client);
        ESP_LOGI(TAG, "RFID access: %s (HTTP %d)", authorized ? "AUTHORIZED" : "DENIED", status);
    }

    // Kiểm tra offline trong bộ nhớ NVS nếu server không khả dụng
    if (!authorized) {
        nvs_handle_t nvs;
        if (nvs_open("rfid_cards", NVS_READONLY, &nvs) == ESP_OK) {
            char key[16];
            snprintf(key, sizeof(key), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
            uint8_t auth_val = 0;
            if (nvs_get_u8(nvs, key, &auth_val) == ESP_OK && auth_val == 1) {
                authorized = true;
                ESP_LOGI(TAG, "RFID access granted offline via NVS cache");
            }
            nvs_close(nvs);
        }
    }

    return authorized;
}

static void register_rfid_uid_to_server(const uint8_t *uid)
{
    char url[192];
    char post_data[128];

    snprintf(url, sizeof(url), "%s/api/device/rfid/enroll", g_server_url);
    snprintf(post_data, sizeof(post_data),
             "{\"device_id\":\"%s\",\"uid\":\"%02X %02X %02X %02X\"}",
             "esp32s3cam", uid[0], uid[1], uid[2], uid[3]);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 4000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
}

static void save_rfid_uid_to_nvs(const uint8_t *uid)
{
    nvs_handle_t nvs;
    if (nvs_open("rfid_cards", NVS_READWRITE, &nvs) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof(key), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
        nvs_set_u8(nvs, key, 1);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Saved UID %02X %02X %02X %02X to local NVS", uid[0], uid[1], uid[2], uid[3]);
    }
    register_rfid_uid_to_server(uid);
}

// =========================================================================
// TASK 2: rfid_task (FUN_42012970) - Quét thẻ từ RC522 & xác thực
// =========================================================================
static void rfid_task(void *pvParameters)
{
    uint8_t last_uid[4] = {0};
    uint8_t current_uid[4] = {0};
    int empty_scans = 0;

    while (true) {
        bool card_present = rc522_read_uid(current_uid);

        if (!card_present || (memcmp(current_uid, last_uid, 4) == 0 && !g_rfid_enroll_mode)) {
            empty_scans++;
            if (empty_scans > 3) {
                memset(last_uid, 0, 4);
                empty_scans = 0;
            }
        } else {
            memcpy(last_uid, current_uid, 4);
            ESP_LOGI(TAG, "RFID UID detected: %02X %02X %02X %02X",
                     current_uid[0], current_uid[1], current_uid[2], current_uid[3]);

            if (g_rfid_enroll_mode) {
                save_rfid_uid_to_nvs(current_uid);
                g_rfid_enroll_mode = false;
                empty_scans = 0;
            } else {
                if (is_rfid_uid_authorized(current_uid)) {
                    g_door_unlocked = true;
                    g_door_unlock_until = esp_timer_get_time() + 20000000ULL; // Mở 20 giây khi quẹt thẻ
                    apply_relay_outputs();
                    ESP_LOGI(TAG, "Door unlocked for 20s via RFID");
                }
                empty_scans = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

// =========================================================================
// TASK 3: relay_discovery_task (LAB_42012618) - Lắng nghe UDP tìm Server nội bộ
// =========================================================================
static void relay_discovery_task(void *pvParameters)
{
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Relay discovery socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(3333),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGW(TAG, "Relay discovery socket bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char rx_buf[160];
    char discovered_url[128];

    while (true) {
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0, NULL, NULL);
        if (len > 0) {
            rx_buf[len] = '\0';
            memset(discovered_url, 0, sizeof(discovered_url));
            if (sscanf(rx_buf, "SMART_HOME_RELAY %127s", discovered_url) == 1) {
                if ((strncmp(discovered_url, "http://", 7) == 0 ||
                     strncmp(discovered_url, "https://", 8) == 0) &&
                    strcmp(discovered_url, g_server_url) != 0) {
                    strncpy(g_server_url, discovered_url, sizeof(g_server_url) - 1);
                    g_server_url[sizeof(g_server_url) - 1] = '\0';
                    save_server_url_to_nvs(g_server_url);
                    ESP_LOGI(TAG, "Relay server discovered: %s", g_server_url);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// =========================================================================
// TASK 4: server_sync_task (LAB_420121bc) - Đồng bộ dữ liệu & nhận lệnh Server
// =========================================================================
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (!esp_http_client_is_chunked_response(evt->client) && evt->user_data) {
            char *buf = (char *)evt->user_data;
            int copy_len = evt->data_len;
            if (resp_len + copy_len < 259) {
                memcpy(buf + resp_len, evt->data, copy_len);
                resp_len += copy_len;
                buf[resp_len] = '\0';
            }
        }
    }
    return ESP_OK;
}

static void server_sync_task(void *pvParameters)
{
    char post_data[512];
    char url[192];
    char resp_buf[260];
    char status_resp[260];

    while (true) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        // 1. Gửi POST JSON trạng thái cảm biến lên <g_server_url>/api/device/status
        int json_len = snprintf(post_data, sizeof(post_data),
            "{\"device_id\":\"%s\",\"pir\":%s,\"gas\":%s,\"door_sensor\":%s,\"door\":%s,\"fan\":%s,\"lamp\":%s,\"temp\":%.1f,\"hum\":%.1f,\"uptime\":%lld}",
            "esp32s3cam",
            g_pir_detected ? "true" : "false",
            g_gas_detected ? "true" : "false",
            g_sensor_3     ? "true" : "false",
            g_door_unlocked ? "true" : "false",
            g_fan_on       ? "true" : "false",
            g_lamp_on      ? "true" : "false",
            g_temperature, g_humidity, (long long)(esp_timer_get_time() / 1000));

        snprintf(url, sizeof(url), "%s/api/device/status", g_server_url);
        memset(status_resp, 0, sizeof(status_resp));
        resp_len = 0;

        esp_http_client_config_t post_cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 4000,
            .event_handler = http_event_handler,
            .user_data = status_resp,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&post_cfg);
        if (client != NULL) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, post_data, json_len);
            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            if (err == ESP_OK) {
                if (status == 200) {
                    ESP_LOGI(TAG, "Server status sent: HTTP 200");
                } else {
                    ESP_LOGW(TAG, "Server status sent: HTTP %d, resp: %s", status, status_resp);
                }
            } else {
                ESP_LOGW(TAG, "Server status failed: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }

        // 2. Gửi GET lấy lệnh điều khiển từ <g_server_url>/api/device/command?device_id=esp32s3cam
        snprintf(url, sizeof(url), "%s/api/device/command?device_id=%s", g_server_url, "esp32s3cam");
        memset(resp_buf, 0, sizeof(resp_buf));
        resp_len = 0;

        esp_http_client_config_t get_cfg = {
            .url = url,
            .timeout_ms = 4000,
            .event_handler = http_event_handler,
            .user_data = resp_buf,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        client = esp_http_client_init(&get_cfg);
        if (client != NULL) {
            if (esp_http_client_perform(client) == ESP_OK && (resp_len > 0 || strlen(resp_buf) > 0)) {
                int val = 0;
                char *p;
                if ((p = strstr(resp_buf, "\"lamp\":")) && sscanf(p, "\"lamp\":%d", &val) == 1) {
                    bool new_lamp = (val != 0);
                    if (new_lamp != g_lamp_on) {
                        g_lamp_on = new_lamp;
                        ESP_LOGI(TAG, "Lamp command: %s (Relay 1 - GPIO 47 / LE1)", g_lamp_on ? "ON" : "OFF");
                    }
                }
                if ((p = strstr(resp_buf, "\"door\":")) && sscanf(p, "\"door\":%d", &val) == 1) {
                    if (val == 1) {
                        g_door_unlocked = true;
                        g_door_unlock_until = esp_timer_get_time() + 20000000ULL; // Tự động khóa sau đúng 20 giây
                        apply_relay_outputs();
                        ESP_LOGI(TAG, "Door command: UNLOCKED for 20s (Relay 2 - GPIO 21 / LE2)");
                    } else if (val == 0 && g_door_unlocked) {
                        g_door_unlocked = false;
                        g_door_unlock_until = 0;
                        apply_relay_outputs();
                        ESP_LOGI(TAG, "Door command: LOCKED (Relay 2 - GPIO 21 / LE2)");
                    }
                }
                if ((p = strstr(resp_buf, "\"fan\":")) && sscanf(p, "\"fan\":%d", &val) == 1) {
                    bool new_fan = (val != 0);
                    if (new_fan != g_fan_on) {
                        g_fan_on = new_fan;
                        ESP_LOGI(TAG, "Fan command: %s (Relay 3 - GPIO 20 / LE3)", g_fan_on ? "ON" : "OFF");
                    }
                }
                if ((p = strstr(resp_buf, "\"buzzer\":")) && sscanf(p, "\"buzzer\":%d", &val) == 1) {
                    bool new_buzzer = (val != 0);
                    if (new_buzzer != g_buzzer_on) {
                        g_buzzer_on = new_buzzer;
                        g_buzzer_manual_override = true;
                        ESP_LOGI(TAG, "Buzzer command: %s (GPIO 48 / Onboard LED)", g_buzzer_on ? "ON" : "OFF");
                    }
                }
                if ((p = strstr(resp_buf, "\"rfid_enroll\":")) && sscanf(p, "\"rfid_enroll\":%d", &val) == 1) {
                    g_rfid_enroll_mode = (val != 0);
                    ESP_LOGI(TAG, "RFID enrollment mode: %s", g_rfid_enroll_mode ? "ON" : "OFF");
                }
                apply_relay_outputs();
            }
            esp_http_client_cleanup(client);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// =========================================================================
// TASK 5: camera_stream_task - Đẩy luồng video thời gian thực lên Cloud
// =========================================================================
static void camera_stream_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Camera real-time stream task started");
    char url[192];
    esp_http_client_handle_t client = NULL;
    bool first_ok = true;

    while (true) {
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "esp_camera_fb_get failed (camera init error?)");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (client == NULL) {
            snprintf(url, sizeof(url), "%s/api/device/camera/upload", g_server_url);
            esp_http_client_config_t cfg = {
                .url = url,
                .method = HTTP_METHOD_POST,
                .timeout_ms = 4000,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .keep_alive_enable = true,
            };
            client = esp_http_client_init(&cfg);
            if (!client) {
                ESP_LOGE(TAG, "Failed to initialize HTTP client for camera upload");
                esp_camera_fb_return(fb);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        esp_http_client_set_header(client, "Content-Type", "image/jpeg");
        esp_http_client_set_header(client, "X-Device-Id", "esp32s3cam");
        esp_http_client_set_post_field(client, (const char *)fb->buf, fb->len);

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);

        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "Upload fail: %s, HTTP %d, len=%u, free heap: int=%u, spiram=%u",
                     esp_err_to_name(err), status, (unsigned)fb->len,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            esp_http_client_cleanup(client);
            client = NULL;
            first_ok = true;
        } else if (first_ok) {
            ESP_LOGI(TAG, "Camera upload OK (%u bytes)", (unsigned)fb->len);
            first_ok = false;
        }

        esp_camera_fb_return(fb);

        // Khoảng nghỉ giữa các frame (~10 FPS)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// =========================================================================
// HÀM CHÍNH KHỞI CHẠY HỆ THỐNG (app_main)
// =========================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-S3 Smart Home Camera System...");

    // 1. Khởi tạo Flash NVS (Non-Volatile Storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Khởi tạo phần cứng và ngoại vi
    init_pins();          // Cấu hình chân GPIO Output & Input
    init_rfid();          // Khởi tạo SPI và cấu hình module MFRC522
    init_camera();        // Khởi tạo camera OV2640
    init_wifi();          // Khởi tạo kết nối Wi-Fi Station
    load_server_config(); // Tải URL Server từ NVS

    // Chờ kết nối Wi-Fi tối đa 15 giây
    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));

    // 3. Khởi tạo 5 FreeRTOS Task chạy song song
    xTaskCreatePinnedToCore(control_task,         "control",         4096, NULL, 5, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(rfid_task,            "rfid",            4096, NULL, 4, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(relay_discovery_task, "relay_discovery", 4096, NULL, 3, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(server_sync_task,     "server_sync",     6144, NULL, 3, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(camera_stream_task,   "camera_stream",   8192, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "ESP32-S3 CAM control system initialized successfully!");
}
