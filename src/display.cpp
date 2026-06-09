/*
 * AXS15231B QSPI display driver for JC3248W535C
 * Written by Alun Morris and Claude Code
 *
 * 110326 AXS15231B QSPI display driver for JC3248W535C
 * 130326 Landscape 480x320 via software rotation in flush callback
 */
#include "display.h"
#include <Arduino.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_types.h>
#include <driver/spi_master.h>
#include <driver/ledc.h>
#include "esp_lcd_axs15231b.h"
#include <freertos/semphr.h>

// ---- Pin definitions (JC3248W535C) ----
#define LCD_BL      1
#define LCD_PCLK    47
#define LCD_CS      45
#define LCD_DATA0   21   // MOSI / SPI D0
#define LCD_DATA1   48   // MISO / SPI D1
#define LCD_DATA2   40   // WP   / SPI D2
#define LCD_DATA3   39   // HD   / SPI D3

// Physical portrait panel: 320 wide x 480 tall.
// Hardware MADCTL rotation is NOT used (corrupts write window in QSPI mode).
// Instead, LVGL renders landscape 480x320 into PSRAM, and the flush callback
// rotates pixels into portrait strips for DMA to the display.
#define LCD_H_RES   320   // physical portrait width
#define LCD_V_RES   480   // physical portrait height
#define LANDSCAPE_W 480   // LVGL landscape width
#define LANDSCAPE_H 320   // LVGL landscape height

// Portrait rows per DMA strip — 320 * 60 * 2 = 38,400 bytes internal RAM
#define DMA_CHUNK_ROWS 60

static lv_color_t            *buf1         = nullptr;   // PSRAM landscape framebuffer
static lv_color_t            *s_dma_out    = nullptr;   // internal DMA portrait strip
static lv_disp_draw_buf_t     draw_buf;
static lv_disp_drv_t          disp_drv;
static esp_lcd_panel_handle_t  panel_handle = nullptr;

// AXS15231B init sequence for JC3248W535C (from Francisrjs reference)
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xBB, (uint8_t []){0x00,0x00,0x00,0x00,0x00,0x00,0x5A,0xA5}, 8, 0},
    {0xA0, (uint8_t []){0xC0,0x10,0x00,0x02,0x00,0x00,0x04,0x3F,0x20,0x05,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00}, 17, 0},
    {0xA2, (uint8_t []){0x30,0x3C,0x24,0x14,0xD0,0x20,0xFF,0xE0,0x40,0x19,0x80,0x80,0x80,0x20,0xf9,0x10,0x02,0xff,0xff,0xF0,0x90,0x01,0x32,0xA0,0x91,0xE0,0x20,0x7F,0xFF,0x00,0x5A}, 31, 0},
    {0xD0, (uint8_t []){0xE0,0x40,0x51,0x24,0x08,0x05,0x10,0x01,0x20,0x15,0x42,0xC2,0x22,0x22,0xAA,0x03,0x10,0x12,0x60,0x14,0x1E,0x51,0x15,0x00,0x8A,0x20,0x00,0x03,0x3A,0x12}, 30, 0},
    {0xA3, (uint8_t []){0xA0,0x06,0xAa,0x00,0x08,0x02,0x0A,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00,0x55,0x55}, 22, 0},
    {0xC1, (uint8_t []){0x31,0x04,0x02,0x02,0x71,0x05,0x24,0x55,0x02,0x00,0x41,0x00,0x53,0xFF,0xFF,0xFF,0x4F,0x52,0x00,0x4F,0x52,0x00,0x45,0x3B,0x0B,0x02,0x0d,0x00,0xFF,0x40}, 30, 0},
    {0xC3, (uint8_t []){0x00,0x00,0x00,0x50,0x03,0x00,0x00,0x00,0x01,0x80,0x01}, 11, 0},
    {0xC4, (uint8_t []){0x00,0x24,0x33,0x80,0x00,0xea,0x64,0x32,0xC8,0x64,0xC8,0x32,0x90,0x90,0x11,0x06,0xDC,0xFA,0x00,0x00,0x80,0xFE,0x10,0x10,0x00,0x0A,0x0A,0x44,0x50}, 29, 0},
    {0xC5, (uint8_t []){0x18,0x00,0x00,0x03,0xFE,0x3A,0x4A,0x20,0x30,0x10,0x88,0xDE,0x0D,0x08,0x0F,0x0F,0x01,0x3A,0x4A,0x20,0x10,0x10,0x00}, 23, 0},
    {0xC6, (uint8_t []){0x05,0x0A,0x05,0x0A,0x00,0xE0,0x2E,0x0B,0x12,0x22,0x12,0x22,0x01,0x03,0x00,0x3F,0x6A,0x18,0xC8,0x22}, 20, 0},
    {0xC7, (uint8_t []){0x50,0x32,0x28,0x00,0xa2,0x80,0x8f,0x00,0x80,0xff,0x07,0x11,0x9c,0x67,0xff,0x24,0x0c,0x0d,0x0e,0x0f}, 20, 0},
    {0xC9, (uint8_t []){0x33,0x44,0x44,0x01}, 4, 0},
    {0xCF, (uint8_t []){0x2C,0x1E,0x88,0x58,0x13,0x18,0x56,0x18,0x1E,0x68,0x88,0x00,0x65,0x09,0x22,0xC4,0x0C,0x77,0x22,0x44,0xAA,0x55,0x08,0x08,0x12,0xA0,0x08}, 27, 0},
    {0xD5, (uint8_t []){0x40,0x8E,0x8D,0x01,0x35,0x04,0x92,0x74,0x04,0x92,0x74,0x04,0x08,0x6A,0x04,0x46,0x03,0x03,0x03,0x03,0x82,0x01,0x03,0x00,0xE0,0x51,0xA1,0x00,0x00,0x00}, 30, 0},
    {0xD6, (uint8_t []){0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE,0x93,0x00,0x01,0x83,0x07,0x07,0x00,0x07,0x07,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x84,0x00,0x20,0x01,0x00}, 30, 0},
    {0xD7, (uint8_t []){0x03,0x01,0x0b,0x09,0x0f,0x0d,0x1E,0x1F,0x18,0x1d,0x1f,0x19,0x40,0x8E,0x04,0x00,0x20,0xA0,0x1F}, 19, 0},
    {0xD8, (uint8_t []){0x02,0x00,0x0a,0x08,0x0e,0x0c,0x1E,0x1F,0x18,0x1d,0x1f,0x19}, 12, 0},
    {0xD9, (uint8_t []){0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}, 12, 0},
    {0xDD, (uint8_t []){0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F}, 12, 0},
    {0xDF, (uint8_t []){0x44,0x73,0x4B,0x69,0x00,0x0A,0x02,0x90}, 8, 0},
    {0xE0, (uint8_t []){0x3B,0x28,0x10,0x16,0x0c,0x06,0x11,0x28,0x5c,0x21,0x0D,0x35,0x13,0x2C,0x33,0x28,0x0D}, 17, 0},
    {0xE1, (uint8_t []){0x37,0x28,0x10,0x16,0x0b,0x06,0x11,0x28,0x5C,0x21,0x0D,0x35,0x14,0x2C,0x33,0x28,0x0F}, 17, 0},
    {0xE2, (uint8_t []){0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D}, 17, 0},
    {0xE3, (uint8_t []){0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x32,0x2F,0x0F}, 17, 0},
    {0xE4, (uint8_t []){0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D}, 17, 0},
    {0xE5, (uint8_t []){0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0F}, 17, 0},
    {0xA4, (uint8_t []){0x85,0x85,0x95,0x82,0xAF,0xAA,0xAA,0x80,0x10,0x30,0x40,0x40,0x20,0xFF,0x60,0x30}, 16, 0},
    {0xA4, (uint8_t []){0x85,0x85,0x95,0x85}, 4, 0},
    {0xBB, (uint8_t []){0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, 8, 0},
    {0x13, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x2C, (uint8_t []){0x00,0x00,0x00,0x00}, 4, 0},
};

// Semaphore to synchronize DMA completion with flush callback
static SemaphoreHandle_t s_flush_done = nullptr;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                           esp_lcd_panel_io_event_data_t *edata,
                                           void *user_ctx) {
    BaseType_t woken = pdFALSE;
    if (s_flush_done) xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

// Landscape→portrait rotation flush.
// LVGL renders 480×320 landscape into PSRAM.  This callback rotates pixels
// into 320-wide portrait strips in internal DMA RAM and sends them to the
// AXS15231B top-to-bottom (RAMWR at y=0, then RAMWRC continuing).
// CW 90°: portrait (px, py) = (319 - ly, lx)
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_map) {
    for (int py_start = 0; py_start < LCD_V_RES; py_start += DMA_CHUNK_ROWS) {
        int py_end = py_start + DMA_CHUNK_ROWS;
        if (py_end > LCD_V_RES) py_end = LCD_V_RES;

        // Rotate: iterate landscape rows (ly) for source cache-friendliness.
        // Each landscape row ly maps to portrait column px = 319 - ly.
        for (int ly = 0; ly < LANDSCAPE_H; ly++) {
            int px = (LANDSCAPE_H - 1) - ly;
            const lv_color_t *src_row = &color_map[ly * LANDSCAPE_W];
            for (int py = py_start; py < py_end; py++) {
                int lx = py;  // portrait row = landscape x
                s_dma_out[(py - py_start) * LCD_H_RES + px] = src_row[lx];
            }
        }

        xSemaphoreTake(s_flush_done, 0);
        esp_lcd_panel_draw_bitmap(panel_handle,
                                  0, py_start, LCD_H_RES, py_end,
                                  s_dma_out);
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
    }

    lv_disp_flush_ready(drv);
}

static void backlight_init() {
    const ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bl_timer);

    const ledc_channel_config_t bl_ch = {
        .gpio_num   = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&bl_ch);
}

void display_backlight_set(int percent) {
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void display_init() {
    s_flush_done = xSemaphoreCreateBinary();
    backlight_init();
    display_backlight_set(100);  // on immediately — if screen stays dark, we're crashing below

    // Init SPI bus with QSPI pins (data0-3 + clock).
    // SPICOMMON_BUSFLAG_QUAD enables WP/HD pins as quad data lines.
    spi_bus_config_t buscfg = {};
    buscfg.data0_io_num   = LCD_DATA0;
    buscfg.data1_io_num   = LCD_DATA1;
    buscfg.sclk_io_num    = LCD_PCLK;
    buscfg.data2_io_num   = LCD_DATA2;
    buscfg.data3_io_num   = LCD_DATA3;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    buscfg.flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Panel IO — lcd_cmd_bits=32 enables QSPI opcode packing in axs15231b driver
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    const esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS, on_color_trans_done, &disp_drv);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                              &io_config, &io_handle));

    // AXS15231B panel
    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds      = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags          = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io_handle, &panel_config, &panel_handle));

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    // No hardware rotation — keep native portrait 320x480.
    // LVGL software rotation is used instead (set in display_lvgl_init).

    esp_lcd_panel_disp_on_off(panel_handle, true);

    display_backlight_set(100);
    Serial.println("Display initialised (AXS15231B QSPI 320x480 portrait)");
}

void display_lvgl_init() {
    // Full landscape frame in PSRAM — LVGL draws here (no DMA involved in drawing)
    size_t fb_pixels = LANDSCAPE_W * LANDSCAPE_H;
    buf1 = (lv_color_t *)heap_caps_malloc(fb_pixels * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(buf1);

    // Portrait DMA output strip — internal RAM for SPI2 DMA
    size_t dma_size = LCD_H_RES * DMA_CHUNK_ROWS * sizeof(lv_color_t);
    s_dma_out = (lv_color_t *)heap_caps_malloc(dma_size,
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_dma_out);

    lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, fb_pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res      = LANDSCAPE_W;  // 480
    disp_drv.ver_res      = LANDSCAPE_H;  // 320
    disp_drv.flush_cb     = lvgl_flush_cb;
    disp_drv.draw_buf     = &draw_buf;
    disp_drv.full_refresh = 1;            // always full frame for rotation
    lv_disp_drv_register(&disp_drv);
    Serial.println("LVGL display driver registered (480x320 landscape, sw rotation)");
}

static inline uint16_t swap16(uint16_t v) { return (v >> 8) | (v << 8); }

// One-chunk internal DMA buffer for fill_solid/test — reused, never freed.
static uint16_t *s_dma_chunk = nullptr;

static void fill_solid(uint16_t colour) {
    if (!s_dma_chunk) {
        s_dma_chunk = (uint16_t *)heap_caps_malloc(
            LCD_H_RES * DMA_CHUNK_ROWS * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_dma_chunk) { Serial.println("fill_solid: DMA alloc failed"); return; }
    }
    uint16_t c = swap16(colour);
    for (int i = 0; i < LCD_H_RES * DMA_CHUNK_ROWS; i++) s_dma_chunk[i] = c;

    for (int y = 0; y < LCD_V_RES; y += DMA_CHUNK_ROWS) {
        int y2 = y + DMA_CHUNK_ROWS;
        if (y2 > LCD_V_RES) y2 = LCD_V_RES;
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y2, s_dma_chunk);
        vTaskDelay(pdMS_TO_TICKS(5));  // yield; DMA from internal RAM completes fast
    }
}

void display_clear() { fill_solid(0x0000); }

void display_test_solid_cycle() {
    static const uint16_t cols[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x07FF, 0xF81F};
    static const char    *names[] = {"Red","Green","Blue","White","Cyan","Magenta"};
    int n = sizeof(cols)/sizeof(cols[0]);
    for (int i = 0; ; i = (i+1) % n) {
        Serial.printf("Solid: %s (0x%04X)\n", names[i], cols[i]);
        fill_solid(cols[i]);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void display_test_quadrants() {
    // Reuse s_dma_chunk — allocate if first call
    if (!s_dma_chunk) {
        s_dma_chunk = (uint16_t *)heap_caps_malloc(
            LCD_H_RES * DMA_CHUNK_ROWS * sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_dma_chunk) { Serial.println("TEST: DMA alloc failed"); return; }
    }
    for (int y = 0; y < LCD_V_RES; y += DMA_CHUNK_ROWS) {
        int y2 = y + DMA_CHUNK_ROWS;
        if (y2 > LCD_V_RES) y2 = LCD_V_RES;
        for (int row = y; row < y2; row++) {
            for (int x = 0; x < LCD_H_RES; x++) {
                uint16_t c;
                if      (row < LCD_V_RES/2 && x < LCD_H_RES/2) c = swap16(0xF800);
                else if (row < LCD_V_RES/2 && x >= LCD_H_RES/2) c = swap16(0x07E0);
                else if (row >= LCD_V_RES/2 && x < LCD_H_RES/2) c = swap16(0x001F);
                else                                              c = swap16(0xFFFF);
                s_dma_chunk[(row - y) * LCD_H_RES + x] = c;
            }
        }
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y2, s_dma_chunk);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    Serial.println("Quadrant test drawn");
}
