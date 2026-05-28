// cardputer_hal.cpp — Cardputer hardware implementation
// Implements display rendering, keyboard polling, and audio output

#include "cardputer_hal.h"

#if defined(ARDUINO)
#include <M5Cardputer.h>
#include <SD.h>
#endif

// Global instance
CardputerHAL g_hal;

// ──────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────

uint16_t CardputerHAL::rgba_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (rgb >> 8) | (rgb << 8); // Swap bytes for big-endian display
}

// ──────────────────────────────────────────────────────────────
// begin — hardware initialisation
// ──────────────────────────────────────────────────────────────

bool CardputerHAL::begin()
{
#if defined(ARDUINO)
    // M5Cardputer init (also inits display, keyboard, speaker)
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    // Display setup: rotate landscape
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setColorDepth(16);

    // PICO-8 signature dark blue background (color #1: rgb(17,17,102))
    // RGB565 0x194A ≈ dark blue
    M5Cardputer.Display.fillScreen(0x194A);

    // ── Draw PICO-8 styled border decorations ──────────────────
    // Left border area (X: 0..55)
    M5Cardputer.Display.setTextColor(0xFBAF); // PICO-8 orange (#9)
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.drawString("ZPT8PC", 4, 8, &fonts::Font0);
    M5Cardputer.Display.setTextColor(0xFC60); // PICO-8 light gray (#6)
    M5Cardputer.Display.drawString("LAYER8", 4, 24, &fonts::Font0);

    M5Cardputer.Display.drawString("HW:S3", 4, 42, &fonts::Font0);
    M5Cardputer.Display.drawString("RAM:32K", 4, 54, &fonts::Font0);
    M5Cardputer.Display.drawString("SCR:128", 4, 66, &fonts::Font0);
    M5Cardputer.Display.drawString("x128", 4, 78, &fonts::Font0);

    // Right border area (X: 184..239)
    M5Cardputer.Display.setTextColor(0x07FF); // PICO-8 cyan (#12)
    M5Cardputer.Display.drawString("40 FPS", 188, 8, &fonts::Font0);
    M5Cardputer.Display.setTextColor(0xFC60); // PICO-8 light gray (#6)
    M5Cardputer.Display.drawString("M5STACK", 188, 26, &fonts::Font0);
    M5Cardputer.Display.drawString("CARD", 188, 38, &fonts::Font0);
    M5Cardputer.Display.drawString("PUTER", 188, 50, &fonts::Font0);
    M5Cardputer.Display.setTextColor(0x6324); // PICO-8 pink (#14)
    M5Cardputer.Display.drawString("SD:", 188, 78, &fonts::Font0);

    // Speaker init
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(128);
    m_audio_init = true;

    // SD card (might have been initialized early to save memory)
    if (!m_sd_ok) {
        m_sd_ok = initSD();
    }
    if (m_sd_ok) {
        M5Cardputer.Display.setTextColor(0x6BE0); // PICO-8 green (#2)
        M5Cardputer.Display.drawString("OK", 210, 78, &fonts::Font0);
    } else {
        M5Cardputer.Display.setTextColor(0xF400); // PICO-8 red (#0D)
        M5Cardputer.Display.drawString("NO", 210, 78, &fonts::Font0);
    }

    return true;
#else
    m_sd_ok = true;
    return true;
#endif
}

bool CardputerHAL::initSD()
{
#if defined(ARDUINO)
    // Cardputer SD on SPI bus: MOSI=14, MISO=39, SCK=40, CS=12
    SPI.begin(40, 39, 14, 12);
    return SD.begin(12, SPI);
#else
    return true;
#endif
}

// ──────────────────────────────────────────────────────────────
// renderScreen — display 128×128 RGBA pixels on the LCD
// ──────────────────────────────────────────────────────────────

void CardputerHAL::renderScreen(const uint8_t *rgba_pixels)
{
#if defined(ARDUINO)
    // Convert RGBA → RGB565 into screen buffer
    for (int i = 0; i < 128 * 128; ++i)
    {
        uint8_t r = rgba_pixels[i * 4 + 0];
        uint8_t g = rgba_pixels[i * 4 + 1];
        uint8_t b = rgba_pixels[i * 4 + 2];
        m_screen_buf[i] = rgba_to_rgb565(r, g, b);
    }

    // Push to display (128×128 centered on 240×135)
    M5Cardputer.Display.pushImage(
        PICO8_OFFSET_X,   // x destination
        PICO8_OFFSET_Y,   // y destination
        PICO8_W,          // width  = 128
        PICO8_H,          // height = 128
        m_screen_buf      // RGB565 data
    );
#endif
}

void CardputerHAL::pushScreenBuffer()
{
#if defined(ARDUINO)
    M5Cardputer.Display.pushImage(
        PICO8_OFFSET_X,
        PICO8_OFFSET_Y,
        PICO8_W,
        PICO8_H,
        m_screen_buf
    );
#endif
}

// ──────────────────────────────────────────────────────────────
// update — poll keyboard each frame
// ──────────────────────────────────────────────────────────────

void CardputerHAL::update()
{
#if defined(ARDUINO)
    M5Cardputer.update();
    M5Cardputer.Keyboard.updateKeyList();
    M5Cardputer.Keyboard.updateKeysState();

    uint32_t now = millis();

    // Build a raw "currently pressed" state from keyboard scan
    bool raw[2][8] = {};

    // ── 1. Check keysState for character-mapped keys ──────────
    {
        Keyboard_Class::KeysState& ks = M5Cardputer.Keyboard.keysState();

        // Check explicit flags from KeysState (Enter, Space, etc.)
        if (ks.enter)
        {
            raw[0][4] = true; // O button (Enter)
            if (!m_enter_held)
            {
                m_enter_held = true;
                int next = (m_text_tail + 1) % (int)sizeof(m_text_queue);
                if (next != m_text_head)
                {
                    m_text_queue[m_text_tail] = '\r';
                    m_text_tail = next;
                }
            }
        }

        else
        {
            m_enter_held = false;
        }

        if (ks.del)
        {
            static uint32_t last_del_time = 0;
            bool should_press = false;
            if (!m_del_held)
            {
                m_del_held = true;
                should_press = true;
                last_del_time = now;
            }
            else if (now - last_del_time > 250)
            {
                should_press = true;
                last_del_time = now - 250 + 50;
            }

            if (should_press)
            {
                int next = (m_text_tail + 1) % (int)sizeof(m_text_queue);
                if (next != m_text_head)
                {
                    m_text_queue[m_text_tail] = '\b';
                    m_text_tail = next;
                }
            }
        }
        else
        {
            m_del_held = false;
        }

        if (ks.space)
        {
            raw[0][5] = true; // X button (Space)
        }

        for (char c : ks.word)
        {
            // Arrow keys: available with OR without Fn
            // Physical labels on Cardputer: ';'=up, '.'=down, ','=left, '/'=right
            // Fn+WASD also sends 0x01-0x04
            if (c == 'w' || c == 'W' || c == 0x01 || c == ';')  raw[0][2] = true; // up
            if (c == 's' || c == 'S' || c == 0x02 || c == '.')  raw[0][3] = true; // down
            if (c == 'a' || c == 'A' || c == 0x03 || c == ',')  raw[0][0] = true; // left
            if (c == 'd' || c == 'D' || c == 0x04 || c == '/')  raw[0][1] = true; // right

            // O button: Enter, Z, K
            if (c == '\n' || c == '\r' || c == 'z' || c == 'Z' || c == 'k' || c == 'K')
                raw[0][4] = true;

            // X button: Space, X, V
            if (c == ' ' || c == 'x' || c == 'X' || c == 'v' || c == 'V')
                raw[0][5] = true;

            // Menu: Esc, Tab, M
            if (c == 0x1b || c == '\t' || c == 'm' || c == 'M')
                raw[0][6] = true;

            // Text input for PICO-8 devkit mode (printable chars only)
            // Only queue text on FIRST press (not on held/repeat)
            if (c >= 32 && c < 127)
            {
                unsigned idx = (unsigned char)c;
                if (!m_text_held[idx])
                {
                    m_text_held[idx] = true;
                    int next = (m_text_tail + 1) % (int)sizeof(m_text_queue);
                    if (next != m_text_head)
                    {
                        m_text_queue[m_text_tail] = c;
                        m_text_tail = next;
                    }
                }
            }
        }

        // Reset held state for keys no longer pressed
        for (int i = 0; i < 256; i++)
        {
            if (i >= 32 && i < 127)
            {
                bool found = false;
                for (char c : ks.word)
                {
                    if ((unsigned char)c == i) { found = true; break; }
                }
                if (!found) m_text_held[i] = false;
            }
        }

        // Fn key alone → Menu/Start
        if (ks.fn) raw[0][6] = true;
    }


    // ── 2. Apply key repeat logic ─────────────────────────────
    for (int player = 0; player < 2; ++player)
    {
        for (int btn = 0; btn < 8; ++btn)
        {
            bool pressed = raw[player][btn];

            if (pressed)
            {
                if (!m_btn_held[player][btn])
                {
                    // Key just pressed this frame → fire immediately
                    m_btn_held[player][btn]      = true;
                    m_btn_press_time[player][btn] = now;
                    m_btn_repeat_time[player][btn] = now;
                    m_buttons[player][btn] = true;
                }
                else
                {
                    // Key held — check repeat timing
                    uint32_t held_ms = now - m_btn_press_time[player][btn];
                    if (held_ms >= KEY_REPEAT_DELAY_MS)
                    {
                        uint32_t since_last = now - m_btn_repeat_time[player][btn];
                        if (since_last >= KEY_REPEAT_INTERVAL_MS)
                        {
                            m_btn_repeat_time[player][btn] = now;
                            m_buttons[player][btn] = true;
                        }
                        else
                        {
                            m_buttons[player][btn] = false;
                        }
                    }
                    else
                    {
                        // Within initial delay — hold active but no repeat
                        m_buttons[player][btn] = false;
                    }
                }
            }
            else
            {
                // Key released
                m_btn_held[player][btn]   = false;
                m_buttons[player][btn]    = false;
            }
        }
    }
#endif
}

// ──────────────────────────────────────────────────────────────
// Audio
// ──────────────────────────────────────────────────────────────

void CardputerHAL::queueAudio(const int16_t *samples, int count)
{
    if (!m_audio_init || count <= 0) return;
#if defined(ARDUINO)
    // M5Cardputer speaker: playRaw(data, length, rate, stereo, repeat, channel)
    // We play non-stereo, 22050 Hz, no repeat, channel 0
    M5Cardputer.Speaker.playRaw(samples, count, 11025, false, 1, 0, false);
#endif
}

void CardputerHAL::tone(int freq, int duration_ms)
{
#if defined(ARDUINO)
    if (m_audio_init)
        M5Cardputer.Speaker.tone(freq, duration_ms);
#endif
}

void CardputerHAL::noTone()
{
#if defined(ARDUINO)
    M5Cardputer.Speaker.stop();
#endif
}

// ──────────────────────────────────────────────────────────────
// Queries
// ──────────────────────────────────────────────────────────────

bool CardputerHAL::getButton(int player, int btn) const
{
    if (player < 0 || player > 1 || btn < 0 || btn > 7) return false;
    return m_buttons[player][btn];
}

bool CardputerHAL::hasText() const
{
    return m_text_head != m_text_tail;
}

char CardputerHAL::getChar()
{
    if (m_text_head == m_text_tail) return 0;
    char c = m_text_queue[m_text_head];
    m_text_head = (m_text_head + 1) % (int)sizeof(m_text_queue);
    return c;
}

float CardputerHAL::getBatteryVoltage() const
{
#if defined(ARDUINO)
    return M5Cardputer.Power.getBatteryVoltage() / 1000.0f;
#else
    return 4.2f;
#endif
}