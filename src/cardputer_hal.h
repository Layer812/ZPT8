// cardputer_hal.h — Cardputer hardware abstraction layer
// Handles display, keyboard input, and audio for the PICO-8 player

#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <functional>

// ──────────────────────────────────────────────────────────────
// Display constants
// ──────────────────────────────────────────────────────────────

// Cardputer display: 240×135 (landscape)
constexpr int LCD_W = 240;
constexpr int LCD_H = 135;

// PICO-8 screen: 128×128 — keep square, scale to 128×128 centered
// Scale factor: 128 fits in 135 → use 128 (no upscale, center with 3.5px border)
// For better use: scale×1 = 128px (fits), or use integer scale
// 135/128 = 1.054 → not integer. Use scale=1 and letterbox OR nearest-neighbour stretch.
// Decision: display 128×128 at 1:1 in center of 240×135 (leaves 56px unused width, 3.5px height)
// For better visual, scale up to fit height: 135/128 ≈ 1.054 → NOT integer.
// Better: nearest-neighbour x1 in 128×128 box centered at x=56, y=3 (3.5 rounded)
constexpr int PICO8_SCALE  = 1;             // integer scale factor
constexpr int PICO8_W      = 128 * PICO8_SCALE;
constexpr int PICO8_H      = 128 * PICO8_SCALE;
constexpr int PICO8_OFFSET_X = (LCD_W - PICO8_W) / 2;  // 56
constexpr int PICO8_OFFSET_Y = (LCD_H - PICO8_H) / 2;  // 3

// ──────────────────────────────────────────────────────────────
// Keyboard mapping
// ──────────────────────────────────────────────────────────────

// Cardputer keyboard physical key labels → PICO-8 button indices:
// btn 0 = left, 1 = right, 2 = up, 3 = down, 4 = O, 5 = X, 6 = menu

struct KeyMap
{
    char key;    // Cardputer key character (from keyboard.isKeyPressed)
    int  btn;    // PICO-8 button index (0-6)
    int  player; // Player 0 or 1
};

// ──────────────────────────────────────────────────────────────
// CardputerHAL class
// ──────────────────────────────────────────────────────────────

class CardputerHAL
{
public:
    CardputerHAL() = default;

    // Call once in setup()
    bool begin();

    bool initSD();

    // Call every frame: polls keyboard, updates display
    void update();

    void renderScreen(const uint8_t *rgba_pixels);
    uint16_t *getScreenBuffer() { return m_screen_buf; }
    void pushScreenBuffer();

    // Audio: queue 16-bit mono PCM samples at 22050 Hz
    // Called from PICO-8 audio callback
    void queueAudio(const uint8_t *samples, int count);

    // Button state query (called per frame from player)
    // Returns true if the PICO-8 button btn is currently pressed
    bool getButton(int player, int btn) const;

    // Keyboard text input (for PICO-8 text/printh)
    bool hasText() const;
    char getChar();

    // SD card: returns true if SD is available
    bool hasSD() const { return m_sd_ok; }

    // Battery / status
    float getBatteryVoltage() const;

    // Beeper tone (simple buzzer fallback)
    void tone(int freq, int duration_ms);
    void noTone();

private:
    void initDisplay();
    void initKeyboard();
    void initAudio();

    // Convert PICO-8 RGBA to RGB565 for the LCD
    static uint16_t rgba_to_rgb565(uint8_t r, uint8_t g, uint8_t b);

    bool m_sd_ok = false;

    // Screen buffer: RGB565 for the 128×128 PICO-8 area
    uint16_t m_screen_buf[128 * 128];

    // Button state: indexed by [player][btn]
    bool m_buttons[2][8] = {};

    // Text input queue
    char m_text_queue[32];
    int  m_text_head = 0;
    int  m_text_tail = 0;

    // Text key held state — prevents duplicate queueing on each poll
    // Indexed by unsigned char value (256 entries)
    bool m_text_held[256] = {};

    // Enter key held state — for queuing '\n' as text input
    bool m_enter_held = false;
    bool m_del_held = false;

    // Audio state
    bool m_audio_init = false;

    // Buzzer state
    bool m_buzzer_active = false;

    // Key repeat state
    // Tracks when each button was first pressed and last repeated
    static constexpr uint32_t KEY_REPEAT_DELAY_MS  = 200; // initial delay before repeat
    static constexpr uint32_t KEY_REPEAT_INTERVAL_MS = 50;  // interval between repeats
    uint32_t m_btn_press_time[2][8] = {};   // millis() when first pressed
    uint32_t m_btn_repeat_time[2][8] = {};  // millis() of last repeat fire
    bool     m_btn_held[2][8] = {};         // currently held down
};

// Global HAL instance
extern CardputerHAL g_hal;
