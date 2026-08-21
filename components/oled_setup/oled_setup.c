#include "oled_setup.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "string.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

/* #include "nvs_config.h" */

#include "nvs.h"
#include "nvs_flash.h"

#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (400 * 1000)
/* Na Heltec WiFi LoRa 32 V3 o OLED onboard tem reset dedicado no
 * GPIO21 (RST_OLED) -- diferente de breakouts genéricos, que não expõem
 * esse pino e usam -1 (reset só por comando). */
#define EXAMPLE_PIN_NUM_RST 21
#define EXAMPLE_I2C_HW_ADDR 0x3C

#define EXAMPLE_LCD_H_RES 128
#define EXAMPLE_LCD_V_RES 64

// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS 8
#define EXAMPLE_LCD_PARAM_BITS 8

#define CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306 1

#define SCREEN_NVS_MEMORY "screen"

static const char TAG[] = "screen";

lv_disp_t *local_disp = NULL;

/* extern void example_lvgl_demo_ui(lv_disp_t *disp); */

/* The LVGL port component calls esp_lcd_panel_draw_bitmap API for send data to
the screen. There must be called lvgl_port_flush_ready(disp) after each
transaction to display. The best way is to use on_color_trans_done callback from
esp_lcd IO config structure. In IDF 5.1 and higher, it is solved inside LVGL
port component. */
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx) {
  lv_disp_t *disp = (lv_disp_t *)user_ctx;
  lvgl_port_flush_ready(disp);
  return false;
}

esp_err_t configure_oled_screen(i2c_port_t *i2c_bus) {
  ESP_LOGI(TAG, "Install panel IO");
  esp_lcd_panel_io_handle_t io_handle = NULL;
  esp_lcd_panel_io_i2c_config_t io_config = {
      .dev_addr = EXAMPLE_I2C_HW_ADDR,
#if USE_I2C_MASTER == 1
      .scl_speed_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
#endif
      .control_phase_bytes = 1,             // According to SSD1306 datasheet
      .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS, // According to SSD1306 datasheet
      .lcd_param_bits =
          EXAMPLE_LCD_PARAM_BITS, // According to SSD1306 datasheet
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
      .dc_bit_offset = 6, // According to SSD1306 datasheet
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
      .dc_bit_offset = 0, // According to SH1107 datasheet
      .flags =
          {
              .disable_control_phase = 1,
          }
#endif
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)*i2c_bus,
                                           &io_config, &io_handle));

  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_config = {
      .bits_per_pixel = 1,
      .reset_gpio_num = EXAMPLE_PIN_NUM_RST,
#if (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0))
      .color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
#endif
  };
#if CONFIG_EXAMPLE_LCD_CONTROLLER_SSD1306
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0))
  esp_lcd_panel_ssd1306_config_t ssd1306_config = {
      .height = EXAMPLE_LCD_V_RES,
  };
  panel_config.vendor_config = &ssd1306_config;
#endif
  ESP_LOGI(TAG, "Install SSD1306 panel driver");
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
  ESP_LOGI(TAG, "Install SH1107 panel driver");
  ESP_ERROR_CHECK(
      esp_lcd_new_panel_sh1107(io_handle, &panel_config, &panel_handle));
#endif

  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if CONFIG_EXAMPLE_LCD_CONTROLLER_SH1107
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
#endif

  ESP_LOGI(TAG, "Initialize LVGL");
  const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  lvgl_port_init(&lvgl_cfg);

  const lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = io_handle,
      .panel_handle = panel_handle,
      .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES,
      .double_buffer = true,
      .hres = EXAMPLE_LCD_H_RES,
      .vres = EXAMPLE_LCD_V_RES,
      .monochrome = true,
#if LVGL_VERSION_MAJOR >= 9
      .color_format = LV_COLOR_FORMAT_RGB565,
#endif
      .rotation =
          {
              .swap_xy = false,
              .mirror_x = false,
              .mirror_y = false,
          },
      .flags = {
#if LVGL_VERSION_MAJOR >= 9
          .swap_bytes = false,
#endif
          .sw_rotate = false,
      }};
  local_disp = lvgl_port_add_disp(&disp_cfg);

  /* Register done callback for IO */
  const esp_lcd_panel_io_callbacks_t cbs = {
      .on_color_trans_done = notify_lvgl_flush_ready,
  };
  esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, local_disp);

  // Lock the mutex due to the LVGL APIs are not thread-safe
  if (lvgl_port_lock(0)) {
    /* Rotation of the screen */
    nvs_handle_t nvs_handle;
    int orientation;
    esp_err_t err = nvs_open(SCREEN_NVS_MEMORY, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
      size_t size = sizeof(orientation);
      err = nvs_get_blob(nvs_handle, "orient", &orientation, &size);
      if (err == ESP_OK) {

        lv_disp_set_rotation(local_disp, orientation);
      } else {
        lv_disp_set_rotation(local_disp, LV_DISPLAY_ROTATION_0);
      }
    } else {
      ESP_LOGE(TAG, "Error in opening NVS: %s", esp_err_to_name(err));
      lv_disp_set_rotation(local_disp, LV_DISPLAY_ROTATION_0);
    }

    /* example_lvgl_demo_ui(local_disp); */
    // Release the mutex
    lvgl_port_unlock();
  }

  return ESP_OK;
}
