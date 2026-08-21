#ifndef OLED_PRINTF_H__
#define OLED_PRINTF_H__

#include "esp_err.h"
#include "lvgl.h"

#define MAX_CONSOLE_LINES 10 // Maximum number of lines to display in the console output

void oled_printf_init(lv_disp_t *disp);
void printf_oled(const char *format, ...);

#endif // OLED_PRINTF_H__
