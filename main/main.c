#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "u8g2.h"
#include <string.h>

#define PIN_SDA 3
#define PIN_SCL 4
#define MAX17048_ADDR 0x36
#define REG_VCELL 0x02
#define REG_SOC 0x04
#define OLED_ADDR 0x3D

typedef struct bmp280_results
{
    int pressure;
    int temperature;
    int altitude;
} bmp280_results;

typedef struct battery_data
{
    float voltage;
    float percent;
} battery_data;

static const char *TAG = "survival_gadget";

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t oled_dev;
static i2c_master_dev_handle_t battery_dev;

static uint8_t tx_buf[256];
static size_t tx_len;

static uint8_t byteCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
                      void *arg_ptr);
static uint8_t gpioDelayCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
                           void *arg_ptr);
static void setUpDisplay(u8g2_t *u8g2);
static void addI2cDevice(uint8_t addr, i2c_master_dev_handle_t *out);

static esp_err_t readBatteryRegister(uint8_t reg, uint16_t *out);
static battery_data getBatteryData();
static void drawScreen(u8g2_t *u8g2, bmp280_results *bmp_results,
                       battery_data *battery_data);

void app_main(void)
{
    ESP_LOGI(TAG, "Starting, free heap: %lu",
             (unsigned long)esp_get_free_heap_size());

    u8g2_t u8g2;
    setUpDisplay(&u8g2);
    addI2cDevice(MAX17048_ADDR, &battery_dev);
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Testing");
        // todo: pull data from modules and draw using &u8g2
    }
}

static void setUpDisplay(u8g2_t *u8g2)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    addI2cDevice(OLED_ADDR, &oled_dev);
    u8g2_Setup_sh1107_i2c_128x128_f(u8g2, U8G2_R0, byteCb, gpioDelayCb);
    u8g2_InitDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0);

    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_ncenB08_tr);
    u8g2_DrawStr(u8g2, 0, 20, "Hello World");
    u8g2_SendBuffer(u8g2);
}

static uint8_t byteCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    switch (msg)
    {
    case U8X8_MSG_BYTE_START_TRANSFER:
        tx_len = 0;
        break;
    case U8X8_MSG_BYTE_SEND:
        if (tx_len + arg_int > sizeof(tx_buf))
        {
            ESP_LOGE(TAG, "tx_buf overflow");
            return 0;
        }
        memcpy(tx_buf + tx_len, arg_ptr, arg_int);
        tx_len += arg_int;
        break;
    case U8X8_MSG_BYTE_END_TRANSFER:
    {
        esp_err_t err = i2c_master_transmit(oled_dev, tx_buf, tx_len, 100);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C transmit failed: %s", esp_err_to_name(err));
            return 0;
        }
        break;
    }
    default:
        return 0;
    }
    return 1;
}

static uint8_t gpioDelayCb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int,
                           void *arg_ptr)
{
    switch (msg)
    {
    case U8X8_MSG_DELAY_MILLI:
        vTaskDelay(pdMS_TO_TICKS(arg_int));
        break;
    case U8X8_MSG_DELAY_10MICRO:
        esp_rom_delay_us(10 * arg_int);
        break;
    case U8X8_MSG_DELAY_100NANO:
        esp_rom_delay_us(1);
        break;
    default:
        break;
    }
    return 1;
}

static esp_err_t readBatteryRegister(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    esp_err_t err =
        i2c_master_transmit_receive(battery_dev, &reg, 1, buf, 2, 1000);
    if (err == ESP_OK)
    {
        *out = ((uint16_t)buf[0] << 8) | buf[1]; // big-endian
    }
    return err;
}

static battery_data getBatteryData()
{
    uint16_t vcell, soc;
    if (readBatteryRegister(REG_VCELL, &vcell) == ESP_OK &&
        readBatteryRegister(REG_SOC, &soc) == ESP_OK)
    {
        float volts = vcell * 78.125e-6f; // 78.125 uV per LSB
        float percent = soc / 256.0f;     // high byte = %, low byte = 1/256 %
        ESP_LOGI(TAG, "Voltage: %.2f V, Battery: %.1f %%", volts, percent);
        battery_data result = {.percent = percent, .voltage = volts};
        return result;
    }
    else
    {
        ESP_LOGE(TAG, "Failed to read MAX17048");
        battery_data failed_result = {.percent = -1.0f, .voltage = -1.0f};
        return failed_result;
    }
}

static void addI2cDevice(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, out));
}
