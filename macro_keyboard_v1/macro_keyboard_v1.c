#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/i2c.h"
#include "bsp/board_api.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// Pins
//--------------------------------------------------------------------+

#define NUM_COLS        3
#define NUM_ROWS        3
#define NUM_KEYS        9
#define FN_BUTTON       9

#define COL0_PIN        0
#define COL1_PIN        1
#define COL2_PIN        2
#define ROW0_PIN        3
#define ROW1_PIN        6
#define ROW2_PIN        7

#define I2C_PORT        i2c0
#define I2C_SDA_PIN     4
#define I2C_SCL_PIN     5
#define OLED_ADDR       0x3C
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      (OLED_HEIGHT / 8)
#define FB_SIZE         (OLED_WIDTH * OLED_PAGES)
#define I2C_TIMEOUT_US  5000

/* Engineered board: CLK=GP19, DT=GP20, SW=GP18 (active-low). */
#define ENC_CLK_PIN     19
#define ENC_DT_PIN      20
#define ENC_SW_PIN      18

#define ZONE_TOP_H      12
#define ZONE_MID_H      38
#define ZONE_BOT_H      14
#define ZONE_MID_Y      ZONE_TOP_H
#define ZONE_BOT_Y      (ZONE_TOP_H + ZONE_MID_H)

#define DEBOUNCE_MS     6
#define DEBOUNCE_FN_MS  2
#define SETTLE_US       10
#define HID_DOWN_MS     12
#define HID_UP_MS       8
#define ALT_TAB_HOLD_MS 750
#define ENC_DETENT_STEPS 4
#define OLED_FRAME_MS   40
#define HID_Q_LEN       48

static const uint col_pins[NUM_COLS] = {COL0_PIN, COL1_PIN, COL2_PIN};
static const uint row_pins[NUM_ROWS] = {ROW0_PIN, ROW1_PIN, ROW2_PIN};

/* Physical button numbers at [row][col]. */
static const uint8_t keymap[NUM_ROWS][NUM_COLS] = {
    {1, 2, 3},
    {5, 6, 7},
    {4, 8, 9},
};

enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_CONSUMER = 2
};

typedef enum {
    MODE_DEV = 0,
    MODE_MEDIA,
    MODE_NAV,
    MODE_COUNT
} app_mode_t;

enum {
    HID_KBD = 1,
    HID_CC  = 2
};

typedef struct {
    uint8_t  type;
    uint8_t  gap_ms;
    uint8_t  mod;
    uint8_t  key;
    uint16_t usage;
} hid_step_t;

static uint8_t framebuffer[FB_SIZE];
static app_mode_t g_mode = MODE_DEV;
static bool g_caps = false;
static bool g_num = false;
static bool g_shift_held = false;
static bool g_display_dirty = true;
static bool g_oled_flushing = false;
static uint8_t g_oled_page = 0;
static float g_latency_ms = 0.0f;
static int g_volume = 0;
static bool g_alt_held = false;
static uint32_t g_alt_last_ms = 0;
static int g_enc_dir = 0;
static uint32_t g_enc_dir_ms = 0;

static hid_step_t hid_q[HID_Q_LEN];
static uint8_t hid_q_head = 0;
static uint8_t hid_q_count = 0;
static uint32_t hid_next_ms = 0;

static bool g_raw[NUM_KEYS + 1];
static bool g_stable[NUM_KEYS + 1];
static uint32_t g_edge_ms[NUM_KEYS + 1];

static bool g_enc_raw = false;
static bool g_enc_stable = false;
static uint32_t g_enc_edge_ms = 0;

/* 5x7 glyphs, one byte per column, LSB = top pixel. Covers ' ' .. '~'. */
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* \ */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f */
    {0x08, 0x14, 0x54, 0x54, 0x3C}, /* g */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* k */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* m */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* z */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* { */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* | */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* } */
    {0x08, 0x04, 0x08, 0x10, 0x08}, /* ~ */
};

//--------------------------------------------------------------------+
// OLED
//--------------------------------------------------------------------+

static void oled_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x80, cmd};
    i2c_write_timeout_us(I2C_PORT, OLED_ADDR, buf, 2, false, I2C_TIMEOUT_US);
}

static void oled_init(void) {
    static const uint8_t cmds[] = {
        0xAE, 0x20, 0x00, 0x40, 0xA1, 0xA8, OLED_HEIGHT - 1,
        0xC8, 0xD3, 0x00, 0xDA, 0x12, 0xD5, 0x80, 0xD9, 0xF1,
        0xDB, 0x40, 0x81, 0xCF, 0x8D, 0x14, 0xA4, 0xA6, 0x2E, 0xAF,
    };
    for (size_t i = 0; i < count_of(cmds); i++) {
        oled_cmd(cmds[i]);
    }
}

static void oled_flush_page(uint8_t page) {
    oled_cmd(0x21);
    oled_cmd(0);
    oled_cmd(OLED_WIDTH - 1);
    oled_cmd(0x22);
    oled_cmd(page);
    oled_cmd(page);

    uint8_t packet[1 + OLED_WIDTH];
    packet[0] = 0x40;
    memcpy(&packet[1], &framebuffer[page * OLED_WIDTH], OLED_WIDTH);
    i2c_write_timeout_us(I2C_PORT, OLED_ADDR, packet, sizeof(packet), false, I2C_TIMEOUT_US);
}

static void oled_clear(void) {
    memset(framebuffer, 0, FB_SIZE);
}

static void oled_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    uint8_t *byte = &framebuffer[x + (y / 8) * OLED_WIDTH];
    uint8_t mask = 1u << (y & 7);
    if (on) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

static void oled_fill_rect(int x, int y, int w, int h, bool on) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            oled_pixel(x + xx, y + yy, on);
        }
    }
}

static void oled_char(int x, int y, char c, bool invert) {
    if (c < ' ' || c > '~') {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - ' '];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            bool on = (bits >> row) & 1;
            oled_pixel(x + col, y + row, invert ? !on : on);
        }
    }
}

static void oled_text(int x, int y, const char *s) {
    while (*s) {
        oled_char(x, y, *s++, false);
        x += 6;
    }
}

static void oled_text_badge(int x, int y, const char *s) {
    int n = (int)strlen(s);
    int w = n * 6 + 1;
    oled_fill_rect(x, y - 1, w, 9, true);
    for (int i = 0; i < n; i++) {
        oled_char(x + 1 + i * 6, y, s[i], true);
    }
}

static void oled_text_right(int right, int y, const char *s) {
    int w = (int)strlen(s) * 6 - 1;
    oled_text(right - w, y, s);
}

static void oled_hline_dashed(int y) {
    for (int x = 0; x < OLED_WIDTH; x += 2) {
        oled_pixel(x, y, true);
    }
}

static void oled_vline_dashed(int x, int y0, int y1) {
    for (int y = y0; y <= y1; y += 2) {
        oled_pixel(x, y, true);
    }
}

static void oled_rect(int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) {
        oled_pixel(x + i, y, true);
        oled_pixel(x + i, y + h - 1, true);
    }
    for (int i = 0; i < h; i++) {
        oled_pixel(x, y + i, true);
        oled_pixel(x + w - 1, y + i, true);
    }
}

static void oled_lock_box(int x, int y, bool on) {
    oled_rect(x, y, 7, 7);
    if (on) {
        oled_fill_rect(x + 2, y + 2, 3, 3, true);
    }
}

static void oled_bar(int x, int y, int w, int h, int pct) {
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    oled_rect(x, y, w, h);
    int inner = ((w - 2) * pct) / 100;
    if (inner > 0) {
        oled_fill_rect(x + 1, y + 1, inner, h - 2, true);
    }
}

static void oled_ticks(int x, int y, int dir) {
    oled_text(x, y, "<--|-->");
    if (dir < 0) {
        oled_fill_rect(x, y + 8, 16, 1, true);
    } else if (dir > 0) {
        oled_fill_rect(x + 24, y + 8, 16, 1, true);
    }
}

//--------------------------------------------------------------------+
// USB descriptors (keyboard + consumer control)
//--------------------------------------------------------------------+

#define USB_VID 0xCafe
#define USB_PID 0x4D4B
#define USB_BCD 0x0200

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0103,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

static uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

/* Second HID interface: host app writes volume here (Windows locks the keyboard collection). */
#define REPORT_ID_VOLUME 1
static uint8_t const desc_hid_vendor[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(1, HID_REPORT_ID(REPORT_ID_VOLUME))
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    return (instance == 1) ? desc_hid_vendor : desc_hid_report;
}

enum { ITF_NUM_HID, ITF_NUM_VENDOR, ITF_NUM_TOTAL };
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#define EPNUM_HID         0x81
#define EPNUM_VENDOR_OUT  0x02
#define EPNUM_VENDOR_IN   0x82

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 5),
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_VENDOR, 0, HID_ITF_PROTOCOL_NONE,
                          sizeof(desc_hid_vendor), EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN,
                          CFG_TUD_HID_EP_BUFSIZE, 5)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Macro Keyboard",
    "Macro Keypad v1",
    NULL,
};

static uint16_t _desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    switch (index) {
    case 0:
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;
    case 3: {
        char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(id, sizeof(id));
        chr_count = strlen(id);
        if (chr_count > 32) {
            chr_count = 32;
        }
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = id[i];
        }
        break;
    }
    default:
        if (index >= count_of(string_desc_arr) || !string_desc_arr[index]) {
            return NULL;
        }
        {
            const char *str = string_desc_arr[index];
            chr_count = strlen(str);
            if (chr_count > 32) {
                chr_count = 32;
            }
            for (size_t i = 0; i < chr_count; i++) {
                _desc_str[1 + i] = str[i];
            }
        }
        break;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

static void host_set_volume(uint8_t vol) {
    if (vol > 100) {
        vol = 100;
    }
    if (g_volume != (int)vol) {
        g_volume = vol;
        g_display_dirty = true;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    if (instance == 1 && reqlen >= 1) {
        buffer[0] = (uint8_t)g_volume;
        return 1;
    }
    (void)report_id;
    (void)report_type;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    if (instance == 1 && bufsize >= 1) {
        uint8_t vol = buffer[0];
        if (bufsize >= 2 && buffer[0] == REPORT_ID_VOLUME) {
            vol = buffer[1];
        }
        host_set_volume(vol);
        return;
    }

    if (report_type != HID_REPORT_TYPE_OUTPUT || bufsize < 1) {
        return;
    }

    uint8_t leds = buffer[0];
    if (report_id == REPORT_ID_KEYBOARD || report_id == 0) {
        bool caps = leds & KEYBOARD_LED_CAPSLOCK;
        bool num = leds & KEYBOARD_LED_NUMLOCK;
        if (caps != g_caps || num != g_num) {
            g_caps = caps;
            g_num = num;
            g_display_dirty = true;
        }
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance;
    (void)report;
    (void)len;
}

//--------------------------------------------------------------------+
// Non-blocking HID queue
//--------------------------------------------------------------------+

static uint8_t hid_q_space(void) {
    return (uint8_t)(HID_Q_LEN - hid_q_count);
}

static bool hid_q_push(hid_step_t step) {
    if (hid_q_count >= HID_Q_LEN) {
        return false;
    }
    uint8_t idx = (uint8_t)((hid_q_head + hid_q_count) % HID_Q_LEN);
    hid_q[idx] = step;
    hid_q_count++;
    return true;
}

static void hid_wakeup(void) {
    if (tud_suspended()) {
        tud_remote_wakeup();
    }
}

static void hid_enqueue_key(uint8_t mod, uint8_t key, uint8_t gap_ms) {
    hid_step_t s = {HID_KBD, gap_ms, mod, key, 0};
    hid_q_push(s);
}

static void hid_enqueue_cc(uint16_t usage, uint8_t gap_ms) {
    hid_step_t s = {HID_CC, gap_ms, 0, 0, usage};
    hid_q_push(s);
}

static void hid_tap(uint8_t mod, uint8_t key) {
    if (hid_q_space() < 2) {
        return;
    }
    hid_wakeup();
    hid_enqueue_key(mod, key, HID_DOWN_MS);
    hid_enqueue_key(0, 0, HID_UP_MS);
}

static void hid_consumer(uint16_t usage) {
    if (hid_q_space() < 2) {
        return;
    }
    hid_wakeup();
    hid_enqueue_cc(usage, HID_DOWN_MS);
    hid_enqueue_cc(0, HID_UP_MS);
}

static void hid_type_char(char c) {
    uint8_t key = 0;
    uint8_t mod = 0;

    if (c >= 'a' && c <= 'z') {
        key = (uint8_t)(HID_KEY_A + (c - 'a'));
    } else if (c >= 'A' && c <= 'Z') {
        key = (uint8_t)(HID_KEY_A + (c - 'A'));
        mod = KEYBOARD_MODIFIER_LEFTSHIFT;
    } else if (c >= '1' && c <= '9') {
        key = (uint8_t)(HID_KEY_1 + (c - '1'));
    } else if (c == '0') {
        key = HID_KEY_0;
    } else if (c == ' ') {
        key = HID_KEY_SPACE;
    } else if (c == '\n') {
        key = HID_KEY_ENTER;
    } else {
        return;
    }
    hid_tap(mod, key);
}

static void hid_type(const char *s) {
    size_t n = strlen(s);
    if (hid_q_space() < n * 2) {
        return;
    }
    while (*s) {
        hid_type_char(*s++);
    }
}

static void hid_release_all(void) {
    if (!g_alt_held) {
        return;
    }
    if (hid_q_space() < 1) {
        return;
    }
    hid_enqueue_key(0, 0, HID_UP_MS);
    g_alt_held = false;
}

static void hid_pump(uint32_t now_ms) {
    if (hid_q_count == 0) {
        return;
    }
    if ((int32_t)(now_ms - hid_next_ms) < 0) {
        return;
    }
    if (!tud_mounted() || !tud_hid_ready()) {
        return;
    }

    hid_step_t step = hid_q[hid_q_head];
    hid_q_head = (uint8_t)((hid_q_head + 1) % HID_Q_LEN);
    hid_q_count--;

    if (step.type == HID_KBD) {
        uint8_t keys[6] = {0};
        keys[0] = step.key;
        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, step.mod, step.key ? keys : NULL);
    } else {
        tud_hid_report(REPORT_ID_CONSUMER, &step.usage, 2);
    }
    hid_next_ms = now_ms + step.gap_ms;
}

//--------------------------------------------------------------------+
// Mode actions
//--------------------------------------------------------------------+

static const char *mode_tag(void) {
    switch (g_mode) {
    case MODE_DEV:   return g_shift_held ? "[DEV*]" : "[DEV]";
    case MODE_MEDIA: return g_shift_held ? "[MEDIA*]" : "[MEDIA]";
    case MODE_NAV:   return g_shift_held ? "[NAV*]" : "[NAV]";
    default:         return "[?]";
    }
}

static void encoder_nav_step(int dir, bool tabs) {
    uint8_t mod;
    if (tabs) {
        mod = KEYBOARD_MODIFIER_LEFTCTRL;
        if (dir < 0) {
            mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
        }
        hid_tap(mod, HID_KEY_TAB);
        return;
    }

    mod = KEYBOARD_MODIFIER_LEFTALT;
    if (dir < 0) {
        mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
    }

    if (hid_q_space() < 2) {
        return;
    }
    hid_wakeup();
    hid_enqueue_key(mod, HID_KEY_TAB, HID_DOWN_MS);
    hid_enqueue_key(mod, 0, HID_UP_MS);
    g_alt_held = true;
    g_alt_last_ms = to_ms_since_boot(get_absolute_time());
}

static void encoder_nav_timeout(uint32_t now_ms) {
    if (!g_alt_held) {
        return;
    }
    if (now_ms - g_alt_last_ms >= ALT_TAB_HOLD_MS) {
        hid_release_all();
    }
}

static void add_clamped(int *v, int delta, int lo, int hi) {
    *v += delta;
    if (*v < lo) {
        *v = lo;
    }
    if (*v > hi) {
        *v = hi;
    }
}

static void on_encoder_turn(int dir) {
    g_enc_dir = dir;
    g_enc_dir_ms = to_ms_since_boot(get_absolute_time());
    g_display_dirty = true;

    switch (g_mode) {
    case MODE_DEV:
        if (g_shift_held) {
            hid_tap(0, dir > 0 ? HID_KEY_ARROW_DOWN : HID_KEY_ARROW_UP);
        } else {
            hid_tap(KEYBOARD_MODIFIER_LEFTCTRL,
                    dir > 0 ? HID_KEY_EQUAL : HID_KEY_MINUS);
        }
        break;
    case MODE_MEDIA:
        if (g_shift_held) {
            /* YouTube / several players: ',' / '.' frame step while paused. */
            hid_tap(0, dir > 0 ? HID_KEY_PERIOD : HID_KEY_COMMA);
        } else {
            hid_consumer(dir > 0 ? HID_USAGE_CONSUMER_VOLUME_INCREMENT
                                 : HID_USAGE_CONSUMER_VOLUME_DECREMENT);
            add_clamped(&g_volume, dir > 0 ? 2 : -2, 0, 100);
        }
        break;
    case MODE_NAV:
        encoder_nav_step(dir, g_shift_held);
        break;
    default:
        break;
    }
}

static void on_button(int button) {
    bool fn = g_shift_held;
    hid_release_all();

    switch (g_mode) {
    case MODE_DEV:
        switch (button) {
        case 1: hid_type(fn ? "git diff\n" : "git status\n"); break;
        case 2: hid_type(fn ? "git push\n" : "git pull\n"); break;
        case 3:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_S);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_F);
            }
            break;
        case 4:
            hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                    HID_KEY_B);
            break;
        case 5: hid_tap(0, fn ? HID_KEY_F11 : HID_KEY_F10); break;
        case 6:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_F9);
            } else {
                hid_tap(0, HID_KEY_F9);
            }
            break;
        case 7:
            hid_tap(KEYBOARD_MODIFIER_LEFTCTRL, fn ? HID_KEY_C : HID_KEY_L);
            break;
        case 8:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_GRAVE);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_F5);
            }
            break;
        default: break;
        }
        break;

    case MODE_MEDIA:
        switch (button) {
        case 1:
            hid_consumer(HID_USAGE_CONSUMER_SCAN_PREVIOUS);
            break;
        case 2:
            if (fn) {
                hid_consumer(HID_USAGE_CONSUMER_STOP);
            } else {
                hid_consumer(HID_USAGE_CONSUMER_PLAY_PAUSE);
            }
            break;
        case 3:
            hid_consumer(HID_USAGE_CONSUMER_SCAN_NEXT);
            break;
        case 4:
            hid_tap(0, HID_KEY_ARROW_LEFT);
            break;
        case 5:
            hid_tap(0, HID_KEY_ARROW_RIGHT);
            break;
        case 6:
            if (fn) {
                hid_consumer(HID_USAGE_CONSUMER_AL_CONSUMER_CONTROL_CONFIGURATION);
            } else {
                hid_consumer(HID_USAGE_CONSUMER_MUTE);
            }
            break;
        case 7:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_D);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_M);
            }
            break;
        case 8:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTALT,
                        HID_KEY_R);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_S);
            }
            break;
        default: break;
        }
        break;

    case MODE_NAV:
        switch (button) {
        case 1:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_E);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_TAB);
            }
            break;
        case 2:
            hid_tap(KEYBOARD_MODIFIER_LEFTGUI,
                    fn ? HID_KEY_ARROW_UP : HID_KEY_ARROW_LEFT);
            break;
        case 3:
            hid_tap(KEYBOARD_MODIFIER_LEFTGUI,
                    fn ? HID_KEY_ARROW_DOWN : HID_KEY_ARROW_RIGHT);
            break;
        case 4:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_T);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_T);
            }
            break;
        case 5:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_K);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL, HID_KEY_W);
            }
            break;
        case 6:
            hid_tap(KEYBOARD_MODIFIER_LEFTGUI, fn ? HID_KEY_I : HID_KEY_D);
            break;
        case 7:
            if (fn) {
                hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_ESCAPE);
            } else {
                hid_tap(KEYBOARD_MODIFIER_LEFTALT, HID_KEY_F4);
            }
            break;
        case 8:
            hid_tap(KEYBOARD_MODIFIER_LEFTGUI, fn ? HID_KEY_V : HID_KEY_L);
            break;
        default: break;
        }
        break;

    default:
        break;
    }
}

//--------------------------------------------------------------------+
// UI
//--------------------------------------------------------------------+

static const char *const labels_dev[8] = {
    "1:git stat", "2:git pull", "3:format", "4:build",
    "5:step ovr", "6:brkpoint", "7:term clr", "8:run test"
};
static const char *const labels_dev_fn[8] = {
    "1:git diff", "2:git push", "3:save", "4:re-task",
    "5:step in", "6:clr brk", "7:sigint", "8:terminal"
};

static const char *const labels_media[8] = {
    "1:prev trk", "2:play/pau", "3:next trk", "4:scrb bck",
    "5:scrb fwd", "6:mute out", "7:mic mute", "8:snip scr"
};
static const char *const labels_media_fn[8] = {
    "1:prev trk", "2:stop", "3:next trk", "4:scrb bck",
    "5:scrb fwd", "6:app mus", "7:deafen", "8:rec scr"
};

static const char *const labels_nav[8] = {
    "1:task vu", "2:snap L", "3:snap R", "4:new tab",
    "5:cls tab", "6:desktop", "7:cls win", "8:lock"
};
static const char *const labels_nav_fn[8] = {
    "1:files", "2:maximiz", "3:minimiz", "4:reopen",
    "5:tear tab", "6:settings", "7:task mgr", "8:clipbrd"
};

static const char *const *mode_labels(void) {
    switch (g_mode) {
    case MODE_DEV:   return g_shift_held ? labels_dev_fn : labels_dev;
    case MODE_MEDIA: return g_shift_held ? labels_media_fn : labels_media;
    default:         return g_shift_held ? labels_nav_fn : labels_nav;
    }
}

static void draw_matrix(void) {
    const char *const *labels = mode_labels();
    const int row_y[] = {14, 23, 32, 41};
    const int left_x = 1;
    const int right_x = 65;

    for (int i = 0; i < 4; i++) {
        oled_text(left_x, row_y[i], labels[i]);
        oled_text(right_x, row_y[i], labels[i + 4]);
    }
    oled_vline_dashed(63, ZONE_MID_Y + 1, ZONE_BOT_Y - 2);
}

static void draw_status_bar(void) {
    const char *tag = mode_tag();
    oled_text_badge(1, 2, tag);

    char lat[12];
    if (g_latency_ms < 10.0f) {
        snprintf(lat, sizeof(lat), "%.1fms", (double)g_latency_ms);
    } else {
        snprintf(lat, sizeof(lat), "%.0fms", (double)g_latency_ms);
    }
    int lat_w = (int)strlen(lat) * 6 - 1;
    int lat_x = OLED_WIDTH - 1 - lat_w;
    oled_text(lat_x, 2, lat);

    int lock_x = 1 + (int)strlen(tag) * 6 + 4;
    oled_char(lock_x, 2, 'C', false);
    oled_lock_box(lock_x + 7, 2, g_caps);
    oled_char(lock_x + 16, 2, 'N', false);
    oled_lock_box(lock_x + 23, 2, g_num);
    oled_hline_dashed(ZONE_TOP_H - 1);
}

static void draw_context_bar(void) {
    oled_hline_dashed(ZONE_BOT_Y);
    int y = ZONE_BOT_Y + 3;
    const char *label = "ENC:";
    bool show_bar = false;
    int pct = 0;
    bool show_ticks = false;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    int dir = ((now - g_enc_dir_ms) < 400) ? g_enc_dir : 0;

    switch (g_mode) {
    case MODE_DEV:
        if (g_shift_held) {
            label = "ENC: HIST";
            show_ticks = true;
        } else {
            label = "ENC: ZOOM";
            show_ticks = true;
        }
        break;
    case MODE_MEDIA:
        if (g_shift_held) {
            label = "ENC: SEEK";
            show_ticks = true;
        } else {
            label = "ENC: VOL";
            show_bar = true;
            pct = g_volume;
        }
        break;
    case MODE_NAV:
        label = g_shift_held ? "ENC: TABS" : "ENC: WIN";
        show_ticks = true;
        break;
    default:
        break;
    }

    oled_text(1, y, label);

    if (show_bar) {
        if (pct < 0) {
            pct = 0;
        }
        if (pct > 100) {
            pct = 100;
        }
        oled_bar(56, y, 42, 7, pct);
        char pct_s[10];
        snprintf(pct_s, sizeof(pct_s), "%d%%", pct);
        oled_text_right(OLED_WIDTH - 1, y, pct_s);
    } else if (show_ticks) {
        oled_ticks(64, y, dir);
    }
}

static void ui_draw(void) {
    oled_clear();
    draw_status_bar();
    draw_matrix();
    draw_context_bar();
}

//--------------------------------------------------------------------+
// Input
//--------------------------------------------------------------------+

static void gpio_in_pullup(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

static void matrix_init(void) {
    for (int c = 0; c < NUM_COLS; c++) {
        gpio_init(col_pins[c]);
        gpio_set_dir(col_pins[c], GPIO_OUT);
        gpio_put(col_pins[c], 0);
    }
    for (int r = 0; r < NUM_ROWS; r++) {
        gpio_init(row_pins[r]);
        gpio_set_dir(row_pins[r], GPIO_IN);
        gpio_pull_down(row_pins[r]);
    }
}

/*
 * Diodes point from the switch toward the row (row is the matrix output).
 * Strobe one column high and read the rows (pulled down).
 */
static void matrix_read_raw(bool pressed[NUM_KEYS + 1]) {
    memset(pressed, 0, sizeof(bool) * (NUM_KEYS + 1));
    for (int c = 0; c < NUM_COLS; c++) {
        gpio_put(col_pins[c], 1);
        busy_wait_us(SETTLE_US);
        for (int r = 0; r < NUM_ROWS; r++) {
            if (gpio_get(row_pins[r])) {
                pressed[keymap[r][c]] = true;
            }
        }
        gpio_put(col_pins[c], 0);
    }
}

static void debounce_bit(bool raw, bool *raw_s, bool *stable, uint32_t *edge_ms,
                          uint32_t now_ms, uint32_t db_ms, bool *rose) {
    if (raw != *raw_s) {
        *raw_s = raw;
        *edge_ms = now_ms;
    } else if (raw != *stable && (now_ms - *edge_ms) >= db_ms) {
        bool prev = *stable;
        *stable = raw;
        if (rose && raw && !prev) {
            *rose = true;
        }
    }
}

static int encoder_poll(void) {
    static uint8_t prev = 0;
    static int accum = 0;
    static bool primed = false;
    static const int8_t table[16] = {
        0, -1,  1,  0,
        1,  0,  0, -1,
       -1,  0,  0,  1,
        0,  1, -1,  0
    };

    uint8_t clk = gpio_get(ENC_CLK_PIN) ? 1u : 0u;
    uint8_t dt  = gpio_get(ENC_DT_PIN) ? 1u : 0u;
    uint8_t curr = (uint8_t)((clk << 1) | dt);
    if (!primed) {
        prev = curr;
        primed = true;
        return 0;
    }
    int8_t delta = table[(prev << 2) | curr];
    prev = curr;
    accum += delta;

    if (accum >= ENC_DETENT_STEPS) {
        accum = 0;
        return 1;
    }
    if (accum <= -ENC_DETENT_STEPS) {
        accum = 0;
        return -1;
    }
    return 0;
}

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+

int main(void) {
    board_init();
    const tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL
    };
    tusb_init(BOARD_TUD_RHPORT, &rh_init);
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    oled_init();

    matrix_init();
    gpio_in_pullup(ENC_CLK_PIN);
    gpio_in_pullup(ENC_DT_PIN);
    gpio_in_pullup(ENC_SW_PIN);

    bool first[NUM_KEYS + 1];
    matrix_read_raw(first);
    memcpy(g_raw, first, sizeof(g_raw));
    memcpy(g_stable, first, sizeof(g_stable));
    g_shift_held = g_stable[FN_BUTTON];

    g_enc_raw = !gpio_get(ENC_SW_PIN);
    g_enc_stable = g_enc_raw;

    uint32_t last_ui_ms = 0u - OLED_FRAME_MS;

    while (true) {
        tud_task();

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        uint32_t t0 = time_us_32();

        bool raw[NUM_KEYS + 1];
        matrix_read_raw(raw);

        bool pressed[NUM_KEYS + 1] = {0};
        for (int i = 1; i <= NUM_KEYS; i++) {
            uint32_t db = (i == FN_BUTTON) ? DEBOUNCE_FN_MS : DEBOUNCE_MS;
            bool rose = false;
            debounce_bit(raw[i], &g_raw[i], &g_stable[i], &g_edge_ms[i],
                        now_ms, db, &rose);
            pressed[i] = rose;
        }

        /* FN (B9) uses the raw scan so the modifier has no extra debounce lag. */
        bool fn = raw[FN_BUTTON];
        if (fn != g_shift_held) {
            g_shift_held = fn;
            if (g_alt_held) {
                hid_release_all();
            }
            g_display_dirty = true;
        }

        int turn = encoder_poll();

        bool enc_raw = !gpio_get(ENC_SW_PIN);
        bool enc_click = false;
        debounce_bit(enc_raw, &g_enc_raw, &g_enc_stable, &g_enc_edge_ms,
                     now_ms, DEBOUNCE_MS, &enc_click);

        g_latency_ms = (time_us_32() - t0) / 1000.0f;

        hid_pump(now_ms);

        if (turn) {
            on_encoder_turn(turn);
        }

        if (enc_click) {
            hid_release_all();
            g_mode = (app_mode_t)((g_mode + 1) % MODE_COUNT);
            g_display_dirty = true;
        }

        for (int i = 1; i <= 8; i++) {
            if (pressed[i]) {
                on_button(i);
            }
        }

        encoder_nav_timeout(now_ms);
        hid_pump(now_ms);

        if (g_oled_flushing) {
            oled_flush_page(g_oled_page);
            g_oled_page++;
            if (g_oled_page >= OLED_PAGES) {
                g_oled_flushing = false;
            }
        } else {
            bool periodic = (now_ms - last_ui_ms) >= 250;
            bool paced = (now_ms - last_ui_ms) >= OLED_FRAME_MS;
            if (periodic || (g_display_dirty && paced)) {
                ui_draw();
                g_display_dirty = false;
                g_oled_flushing = true;
                g_oled_page = 0;
                last_ui_ms = now_ms;
            }
        }
    }
}
