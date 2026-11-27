#ifndef SGH_LCD_H
#define SGH_LCD_H
#include "bus.h"

typedef enum { LCD_BACKLIGHT_OFF, LCD_BACKLIGHT_NORMAL, LCD_BACKLIGHT_BRIGHT, LCD_BACKLIGHT_PWM } LcdBacklight;
enum { LCD_LIGHT_SCALE = 256 };

typedef struct {
    Bus *bus;
    uint32_t command_port, data_port, *pixels;
    uint16_t ram[256 * 256];
    unsigned width, height, origin_y, x, y, x_start, x_end, y_start, y_end;
    unsigned command, remaining, parameter, args[4], entry, pattern;
    int enabled, standby, attached, selected, powered, byte_commands;
    uint8_t contrast;
    LcdBacklight backlight;
    unsigned light_gain;
    uint64_t power_changes;
    uint64_t commands, writes, first_write_tick;
} Lcd;

int lcd_attach(Lcd *lcd, Bus *bus, uint32_t command_port, uint32_t data_port, unsigned origin_y);
void lcd_connect(Lcd *lcd, uint32_t *pixels, unsigned width, unsigned height);
void lcd_reset(Lcd *lcd);
/* Enable and boost are the high fractions of the two pins, scaled to LCD_LIGHT_SCALE. */
void lcd_set_backlight(Lcd *lcd, unsigned enable, unsigned boost);
void lcd_cut_power(Lcd *lcd);
int lcd_save_bmp(const Lcd *lcd, const char *path);
#endif
