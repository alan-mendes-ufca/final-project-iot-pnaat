#include "oled_printf.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char TAG[] = "oled_printf";

static lv_obj_t *oled_label = NULL; // Global LVGL label for printf_oled

void oled_printf_init(lv_disp_t *disp) {
  ESP_LOGI(TAG, "Initializing oled_printf with LVGL display.");
  if (lvgl_port_lock(0)) {
    oled_label = lv_label_create(lv_display_get_screen_active(disp));
    lv_obj_align(oled_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(oled_label, "");
    // Set text wrapping so that long lines wrap to the next line
    lv_label_set_long_mode(oled_label, LV_LABEL_LONG_WRAP);
    // Set object width to the display's horizontal resolution for wrapping
    lv_obj_set_width(oled_label, lv_disp_get_hor_res(disp));
    lvgl_port_unlock();
  } else {
    ESP_LOGE(TAG, "Could not acquire LVGL lock to initialize oled_printf.");
  }
}

void printf_oled(const char *format, ...) {
  char new_message_buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(new_message_buffer, sizeof(new_message_buffer), format, args);
  va_end(args);

  if (lvgl_port_lock(0)) {
    if (oled_label) {
      const char *current_text = lv_label_get_text(oled_label);
      char combined_text_buffer[1024]; // Larger buffer for combined text

      // Construct the new string: new_message + '\n' + current_text
      // Ensure the combined string does not exceed buffer size
      int len =
          snprintf(combined_text_buffer, sizeof(combined_text_buffer), "%s\n%s",
                   new_message_buffer, current_text ? current_text : "");

      // Truncate if too many lines
      if (len >= sizeof(combined_text_buffer)) {
        // Handle buffer overflow - this means even before truncation, it was
        // too big. In this case, snprintf will have already truncated it. We
        // should ensure it's null-terminated.
        combined_text_buffer[sizeof(combined_text_buffer) - 1] = '\0';
      }

      int newline_count = 0;
      char *ptr = combined_text_buffer;
      char *truncate_pos = NULL;

      while (*ptr != '\0') {
        if (*ptr == '\n') {
          newline_count++;
          if (newline_count == MAX_CONSOLE_LINES) {
            truncate_pos = ptr; // Mark the newline after the last allowed line
            break;
          }
        }
        ptr++;
      }

      if (truncate_pos != NULL) {
        // Null-terminate right after the last allowed newline
        *(truncate_pos + 1) = '\0';
      }
      // If there are less than MAX_CONSOLE_LINES, no truncation needed.

      lv_label_set_text(oled_label, combined_text_buffer);
    }
    lvgl_port_unlock();
  } else {
    ESP_LOGE(TAG, "Could not acquire LVGL lock to update oled_printf label.");
  }
}
