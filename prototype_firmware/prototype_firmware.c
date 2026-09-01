#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/i2c.h"
#include "bsp/board_api.h"
#include "tusb.h"

#define NUM_BUTTONS     7
#define DEBOUNCE_MS     20

#define I2C_PORT        i2c0
#define I2C_SDA_PIN     4
#define I2C_SCL_PIN     5
#define OLED_ADDR       0x3C
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      (OLED_HEIGHT / 8)
#define FB_SIZE         (OLED_WIDTH * OLED_PAGES)

#define ENC_CLK_PIN     20
#define ENC_DT_PIN      19
#define ENC_SW_PIN      18

#define ZONE_TOP_H      12
#define ZONE_MID_H      38
#define ZONE_BOT_H      14
#define ZONE_MID_Y      ZONE_TOP_H
#define ZONE_BOT_Y      (ZONE_TOP_H + ZONE_MID_H)

#define ALT_TAB_HOLD_MS 750
#define ENC_DETENT_STEPS 4

static const uint button_pins[NUM_BUTTONS] = {0, 1, 2, 3, 6, 7, 8};

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

static uint8_t framebuffer[FB_SIZE];
static app_mode_t g_mode = MODE_DEV;
static bool g_caps = false;
static bool g_num = false;
static bool g_shift_held = false;
static bool g_display_dirty = true;
static float g_latency_ms = 0.0f;
static int g_volume = 50;
static int g_zoom = 50;
static bool g_alt_held = false;
static uint8_t g_alt_mod = 0;
static uint32_t g_alt_last_ms = 0;

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
    i2c_write_blocking(I2C_PORT, OLED_ADDR, buf, 2, false);
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

static void oled_show(void) {
    oled_cmd(0x21);
    oled_cmd(0);
    oled_cmd(OLED_WIDTH - 1);
    oled_cmd(0x22);
    oled_cmd(0);
    oled_cmd(OLED_PAGES - 1);

    static uint8_t packet[1 + FB_SIZE];
    packet[0] = 0x40;
    memcpy(&packet[1], framebuffer, FB_SIZE);
    i2c_write_blocking(I2C_PORT, OLED_ADDR, packet, sizeof(packet), false);
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

static void oled_char(int x, int y, char c) {
    if (c < ' ' || c > '~') {
        c = '?';
    }
    const uint8_t *glyph = font5x7[c - ' '];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            oled_pixel(x + col, y + row, (bits >> row) & 1);
        }
    }
}

static void oled_text(int x, int y, const char *s) {
    while (*s) {
        oled_char(x, y, *s++);
        x += 6;
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

static void oled_fill_rect(int x, int y, int w, int h, bool on) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            oled_pixel(x + xx, y + yy, on);
        }
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
    .bcdDevice          = 0x0100,
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

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

enum { ITF_NUM_HID, ITF_NUM_TOTAL };
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_HID 0x81

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 5)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Macro Keyboard",
    "Prototype Pad",
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

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
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
// HID helpers
//--------------------------------------------------------------------+

static void usb_delay_ms(uint32_t ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < ms) {
        tud_task();
    }
}

static bool hid_wait_ready(uint32_t timeout_ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (!tud_hid_ready()) {
        tud_task();
        if (to_ms_since_boot(get_absolute_time()) - start > timeout_ms) {
            return false;
        }
    }
    return true;
}

static void hid_wakeup(void) {
    if (tud_suspended()) {
        tud_remote_wakeup();
    }
}

static void hid_keyboard(uint8_t mod, uint8_t key) {
    if (!tud_mounted() || !hid_wait_ready(50)) {
        return;
    }
    uint8_t keys[6] = {0};
    keys[0] = key;
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, mod, key ? keys : NULL);
}

static void hid_tap(uint8_t mod, uint8_t key) {
    hid_wakeup();
    hid_keyboard(mod, key);
    usb_delay_ms(12);
    hid_keyboard(0, 0);
    usb_delay_ms(8);
}

static void hid_consumer(uint16_t usage) {
    hid_wakeup();
    if (!tud_mounted() || !hid_wait_ready(50)) {
        return;
    }
    tud_hid_report(REPORT_ID_CONSUMER, &usage, 2);
    usb_delay_ms(12);
    if (hid_wait_ready(50)) {
        uint16_t empty = 0;
        tud_hid_report(REPORT_ID_CONSUMER, &empty, 2);
    }
    usb_delay_ms(8);
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
    while (*s) {
        hid_type_char(*s++);
    }
}

static void hid_release_all(void) {
    if (g_alt_held) {
        hid_keyboard(0, 0);
        g_alt_held = false;
        g_alt_mod = 0;
    }
}

//--------------------------------------------------------------------+
// Mode actions
//--------------------------------------------------------------------+

static bool mode_b7_is_shift(void) {
    return g_mode == MODE_DEV;
}

static const char *mode_tag(void) {
    switch (g_mode) {
    case MODE_DEV:   return g_shift_held ? "[DEV*]" : "[DEV]";
    case MODE_MEDIA: return "[MEDIA]";
    case MODE_NAV:   return "[NAV]";
    default:         return "[?]";
    }
}

static void encoder_nav_step(int dir) {
    uint8_t mod = KEYBOARD_MODIFIER_LEFTALT;
    if (dir < 0) {
        mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
    }

    hid_wakeup();
    hid_keyboard(mod, HID_KEY_TAB);
    usb_delay_ms(12);
    hid_keyboard(mod, 0);
    g_alt_held = true;
    g_alt_mod = mod;
    g_alt_last_ms = to_ms_since_boot(get_absolute_time());
}

static void encoder_nav_timeout(void) {
    if (!g_alt_held) {
        return;
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - g_alt_last_ms >= ALT_TAB_HOLD_MS) {
        hid_keyboard(0, 0);
        g_alt_held = false;
        g_alt_mod = 0;
    }
}

static void on_encoder_turn(int dir) {
    switch (g_mode) {
    case MODE_DEV:
        if (g_shift_held) {
            hid_tap(0, dir > 0 ? HID_KEY_ARROW_DOWN : HID_KEY_ARROW_UP);
        } else {
            hid_tap(KEYBOARD_MODIFIER_LEFTCTRL,
                    dir > 0 ? HID_KEY_EQUAL : HID_KEY_MINUS);
            g_zoom += dir > 0 ? 5 : -5;
            if (g_zoom < 0) {
                g_zoom = 0;
            }
            if (g_zoom > 100) {
                g_zoom = 100;
            }
            g_display_dirty = true;
        }
        break;
    case MODE_MEDIA:
        hid_consumer(dir > 0 ? HID_USAGE_CONSUMER_VOLUME_INCREMENT
                             : HID_USAGE_CONSUMER_VOLUME_DECREMENT);
        g_volume += dir > 0 ? 2 : -2;
        if (g_volume < 0) {
            g_volume = 0;
        }
        if (g_volume > 100) {
            g_volume = 100;
        }
        g_display_dirty = true;
        break;
    case MODE_NAV:
        encoder_nav_step(dir);
        break;
    default:
        break;
    }
}

static void on_button(int index) {
    /* index is 0..6 for B1..B7 */
    switch (g_mode) {
    case MODE_DEV:
        switch (index) {
        case 0: hid_type("git status\n"); break;
        case 1: hid_type("git pull\n"); break;
        case 2: hid_tap(KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_F); break;
        case 3: hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_B); break;
        case 4: hid_tap(0, HID_KEY_F10); break;
        case 5: hid_tap(0, HID_KEY_F9); break;
        default: break; /* B7 is hold-to-shift */
        }
        break;
    case MODE_MEDIA:
        switch (index) {
        case 0: hid_consumer(HID_USAGE_CONSUMER_SCAN_PREVIOUS); break;
        case 1: hid_consumer(HID_USAGE_CONSUMER_PLAY_PAUSE); break;
        case 2: hid_consumer(HID_USAGE_CONSUMER_SCAN_NEXT); break;
        case 3: hid_tap(0, HID_KEY_ARROW_LEFT); break;
        case 4: hid_tap(0, HID_KEY_ARROW_RIGHT); break;
        case 5: hid_tap(KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_S); break;
        case 6: hid_tap(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_LEFTSHIFT,
                        HID_KEY_M); break;
        default: break;
        }
        break;
    case MODE_NAV:
        switch (index) {
        case 0: hid_tap(KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTCTRL,
                        HID_KEY_ARROW_LEFT); break;
        case 1: hid_tap(KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTCTRL,
                        HID_KEY_ARROW_RIGHT); break;
        case 2: hid_tap(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_TAB); break;
        case 3: hid_tap(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_ARROW_LEFT); break;
        case 4: hid_tap(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_ARROW_RIGHT); break;
        case 5: hid_tap(KEYBOARD_MODIFIER_LEFTALT, HID_KEY_F4); break;
        case 6: hid_tap(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_L); break;
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

static const char *const labels_dev[] = {
    "B1: status", "B2: pull", "B3: format",
    "B4: build",  "B5: step", "B6: brkpt"
};

static const char *const labels_media[] = {
    "B1: prev", "B2: play", "B3: next", "B4: -10s",
    "B5: +10s", "B6: snip", "B7: mute"
};

static const char *const labels_nav[] = {
    "B1: desk L", "B2: desk R", "B3: tasks", "B4: snap L",
    "B5: snap R", "B6: close", "B7: lock"
};

static void draw_matrix(void) {
    const char *const *labels;
    int n;
    bool three_plus_three;

    switch (g_mode) {
    case MODE_DEV:
        labels = labels_dev;
        n = 6;
        three_plus_three = true;
        break;
    case MODE_MEDIA:
        labels = labels_media;
        n = 7;
        three_plus_three = false;
        break;
    default:
        labels = labels_nav;
        n = 7;
        three_plus_three = false;
        break;
    }

    const int row_y[] = {15, 24, 33, 42};
    const int left_x = 1;
    const int right_x = 66;

    if (three_plus_three) {
        for (int i = 0; i < 3; i++) {
            oled_text(left_x, row_y[i], labels[i]);
            oled_text(right_x, row_y[i], labels[i + 3]);
        }
        if (g_shift_held) {
            oled_text(left_x, row_y[3], "B7: SHIFT");
            oled_text(right_x, row_y[3], "ENC: hist");
        }
    } else {
        int left_n = 4;
        int right_n = n - left_n;
        for (int i = 0; i < left_n; i++) {
            oled_text(left_x, row_y[i], labels[i]);
        }
        for (int i = 0; i < right_n; i++) {
            oled_text(right_x, row_y[i], labels[left_n + i]);
        }
    }

    oled_vline_dashed(63, ZONE_MID_Y + 1, ZONE_BOT_Y - 2);
}

static void draw_status_bar(void) {
    const char *tag = mode_tag();
    oled_text(1, 2, tag);

    char lat[12];
    if (g_latency_ms < 10.0f) {
        snprintf(lat, sizeof(lat), "LAT:%.1fms", (double)g_latency_ms);
    } else {
        snprintf(lat, sizeof(lat), "LAT:%.0fms", (double)g_latency_ms);
    }

    int lock_x = 1 + (int)strlen(tag) * 6 + 2;
    int lock_end = lock_x + 30;
    int lat_w = (int)strlen(lat) * 6 - 1;
    int lat_x = OLED_WIDTH - 1 - lat_w;
    if (lock_end + 2 > lat_x) {
        if (g_latency_ms < 10.0f) {
            snprintf(lat, sizeof(lat), "%.1fms", (double)g_latency_ms);
        } else {
            snprintf(lat, sizeof(lat), "%.0fms", (double)g_latency_ms);
        }
        lat_w = (int)strlen(lat) * 6 - 1;
        lat_x = OLED_WIDTH - 1 - lat_w;
    }

    oled_char(lock_x, 2, 'C');
    oled_lock_box(lock_x + 7, 2, g_caps);
    oled_char(lock_x + 16, 2, 'N');
    oled_lock_box(lock_x + 23, 2, g_num);
    oled_text(lat_x, 2, lat);
    oled_hline_dashed(ZONE_TOP_H - 1);
}

static void draw_context_bar(void) {
    oled_hline_dashed(ZONE_BOT_Y);
    int y = ZONE_BOT_Y + 4;
    const char *label = "Knob";
    bool show_bar = false;
    int pct = 0;

    switch (g_mode) {
    case MODE_DEV:
        if (g_shift_held) {
            label = "History";
        } else {
            label = "Zoom";
            show_bar = true;
            pct = g_zoom;
        }
        break;
    case MODE_MEDIA:
        label = "Volume";
        show_bar = true;
        pct = g_volume;
        break;
    case MODE_NAV:
        label = "Windows";
        break;
    default:
        break;
    }

    oled_text(1, y, label);

    if (show_bar) {
        oled_bar(50, y, 48, 7, pct);
        char pct_s[8];
        snprintf(pct_s, sizeof(pct_s), "%d%%", pct);
        oled_text_right(OLED_WIDTH - 1, y, pct_s);
    }
}

static void ui_render(void) {
    oled_clear();
    draw_status_bar();
    draw_matrix();
    draw_context_bar();
    oled_show();
}

//--------------------------------------------------------------------+
// Input
//--------------------------------------------------------------------+

static void gpio_in_pullup(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

static int encoder_poll(void) {
    /* Gray-code quadrature; fire once per detent (4 transitions). */
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

    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_in_pullup(button_pins[i]);
    }
    gpio_in_pullup(ENC_CLK_PIN);
    gpio_in_pullup(ENC_DT_PIN);
    gpio_in_pullup(ENC_SW_PIN);

    bool last_btn[NUM_BUTTONS];
    for (int i = 0; i < NUM_BUTTONS; i++) {
        last_btn[i] = gpio_get(button_pins[i]);
    }
    bool last_enc_sw = gpio_get(ENC_SW_PIN);

    uint32_t last_ui_ms = 0;

    while (true) {
        tud_task();

        uint32_t t0 = time_us_32();
        int turn = encoder_poll();
        bool enc_sw_edge = last_enc_sw && !gpio_get(ENC_SW_PIN);
        bool shift = mode_b7_is_shift() && !gpio_get(button_pins[6]);
        int pressed = -1;
        for (int i = 0; i < NUM_BUTTONS; i++) {
            bool current = gpio_get(button_pins[i]);
            if (last_btn[i] && !current) {
                pressed = i;
            }
            last_btn[i] = current;
        }
        g_latency_ms = (time_us_32() - t0) / 1000.0f;

        if (shift != g_shift_held) {
            g_shift_held = shift;
            g_display_dirty = true;
        }

        if (turn) {
            on_encoder_turn(turn);
        }

        if (enc_sw_edge) {
            usb_delay_ms(DEBOUNCE_MS);
            if (!gpio_get(ENC_SW_PIN)) {
                hid_release_all();
                g_mode = (app_mode_t)((g_mode + 1) % MODE_COUNT);
                g_shift_held = false;
                g_display_dirty = true;
            }
        }
        last_enc_sw = gpio_get(ENC_SW_PIN);
        encoder_nav_timeout();

        if (pressed >= 0) {
            usb_delay_ms(DEBOUNCE_MS);
            if (!gpio_get(button_pins[pressed])) {
                if (!(pressed == 6 && mode_b7_is_shift())) {
                    on_button(pressed);
                }
            }
            last_btn[pressed] = gpio_get(button_pins[pressed]);
        }

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (g_display_dirty || (now_ms - last_ui_ms) >= 250) {
            ui_render();
            g_display_dirty = false;
            last_ui_ms = now_ms;
        }
    }
}
