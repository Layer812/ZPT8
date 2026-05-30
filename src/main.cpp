// main.cpp — M5Cardputer PICO-8 player entry point
// Initializes HAL, loads BIOS and cartridges from SD, and runs main emulator loop

#include <Arduino.h>
#include <SD.h>
#include <SPIFFS.h>
#include <M5Unified.h>
#include "pico8/vm.h"
#include "cardputer_hal.h"
#include "bindings/lua.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

z8::pico8::vm *g_vm = nullptr;

// ── Audio double-buffer (FreeRTOS task on Core0) ──────────────
// Buffer A/B: main loop fills "write" side, audio task reads "read" side
// 11025Hz: 1472 samples per user request for maximum audio stability
static constexpr int AUDIO_SAMPLES = 1472;
static constexpr int AUDIO_RATE    = 11025;
static uint8_t g_audio_dbuf[2][AUDIO_SAMPLES];
static volatile int  g_audio_write_idx = 0;   // index main loop writes to
static SemaphoreHandle_t g_audio_sem = nullptr;
static TaskHandle_t g_audio_task_handle = nullptr;

// Audio task: runs on Core0, queues completed buffers to speaker
static void audioTask(void* arg)
{
    float prev_x = 0;
    float prev_y = 0;
    const float R = 0.995f; // DC blocker coefficient to prevent pop noises

    while (true)
    {
        // Wait for signal from main loop (new buffer ready)
        if (xSemaphoreTake(g_audio_sem, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            int read_idx = 1 - g_audio_write_idx; // opposite side

            // Apply DC blocker (high-pass filter) to remove DC offset and smooth out cuts
            for (int i = 0; i < AUDIO_SAMPLES; i++) {
                float x = g_audio_dbuf[read_idx][i] - 128.0f;
                float y = x - prev_x + R * prev_y;
                prev_x = x;
                prev_y = y;
                if (y > 127.0f) y = 127.0f;
                else if (y < -127.0f) y = -127.0f;
                g_audio_dbuf[read_idx][i] = (uint8_t)(y + 128.0f);
            }

            g_hal.queueAudio(g_audio_dbuf[read_idx], AUDIO_SAMPLES);
        }
        else
        {
            // Under-run: feed silence smoothly
            static uint8_t silence[AUDIO_SAMPLES];
            for (int i = 0; i < AUDIO_SAMPLES; i++) {
                float x = 0.0f;
                float y = x - prev_x + R * prev_y;
                prev_x = x;
                prev_y = y;
                if (y > 127.0f) y = 127.0f;
                else if (y < -127.0f) y = -127.0f;
                silence[i] = (uint8_t)(y + 128.0f);
            }
            g_hal.queueAudio(silence, AUDIO_SAMPLES);
        }
    }
}

// Collect all .p8 game files from SD card
static void collect_cartridges(std::vector<std::string>& games, bool include_console = true)
{
    games.clear();

    // Add "Console" as the first option
    if (include_console) {
        games.push_back("__CONSOLE__");
    }

    auto scan_dir = [&](const char* path) {
        File dir = SD.open(path);
        if (!dir || !dir.isDirectory()) return;
        while (true) {
            File entry = dir.openNextFile();
            if (!entry) break;
            std::string name = entry.name();
            entry.close();
            // Skip bios.p8
            if (name == "bios.p8") continue;
            // Check for .pc8c or .p8c only (precompiled)
            if ((name.length() >= 5 && name.substr(name.length() - 5) == ".pc8c") ||
                (name.length() >= 3 && name.substr(name.length() - 3) == ".p8"))
            {
                std::string full_path;
                if (path[0] == '/' && path[1] == '\0')
                    full_path = std::string("/") + name;
                else
                    full_path = std::string(path) + "/" + name;
                games.push_back(full_path);
            }
        }
        dir.close();
    };

    // Scan /cartridges/ first
    scan_dir("/cartridges");
    // Then scan root (avoid duplicates)
    {
        File dir = SD.open("/");
        if (dir && dir.isDirectory()) {
            while (true) {
                File entry = dir.openNextFile();
                if (!entry) break;
                std::string name = entry.name();
                entry.close();
                if (name == "bios.p8") continue;
                if (name == "cartridges") continue; // directory
                if ((name.length() >= 5 && name.substr(name.length() - 5) == ".pc8c") ||
                    (name.length() >= 3 && name.substr(name.length() - 3) == ".p8"))
                {
                    games.push_back(std::string("/") + name);
                }
            }
            dir.close();
        }
    }
}

std::string find_cartridge()
{
    // Check if precompiled game exists first
    if (SD.exists("/cartridges/game.pc8c")) return "/cartridges/game.pc8c";
    if (SD.exists("/cartridges/game.p8c")) return "/cartridges/game.p8c";
    if (SD.exists("/game.pc8c")) return "/game.pc8c";
    if (SD.exists("/game.p8c")) return "/game.p8c";

    // List directory and find the first .p8 or .png
    File dir = SD.open("/cartridges");
    if (dir && dir.isDirectory())
    {
        while (true)
        {
            File entry = dir.openNextFile();
            if (!entry) break;
            std::string name = entry.name();
            if (name == "bios.p8")
            {
                entry.close();
                continue;
            }
            // check suffix safely (.pc8c or .p8c only)
            if ((name.length() >= 5 && name.substr(name.length() - 5) == ".pc8c") ||
                (name.length() >= 3 && name.substr(name.length() - 3) == ".p8"))
            {
                std::string full_path = std::string("/cartridges/") + entry.name();
                entry.close();
                dir.close();
                return full_path;
            }
            entry.close();
        }
        dir.close();
    }

    // Root directory check
    dir = SD.open("/");
    if (dir && dir.isDirectory())
    {
        while (true)
        {
            File entry = dir.openNextFile();
            if (!entry) break;
            std::string name = entry.name();
            if (name == "bios.p8")
            {
                entry.close();
                continue;
            }
            if ((name.length() >= 5 && name.substr(name.length() - 5) == ".pc8c") ||
                (name.length() >= 3 && name.substr(name.length() - 3) == ".p8"))
            {
                std::string full_path = std::string("/") + entry.name();
                entry.close();
                dir.close();
                return full_path;
            }
            entry.close();
        }
        dir.close();
    }

    return "";
}

struct GameItem
{
    std::string path;
    std::string list_name;
    std::string right_name;
    std::string right_author;
};

// Simple game selector UI — returns selected cartridge path or "" if cancelled
static std::string show_game_selector()
{
    std::vector<std::string> games;
    collect_cartridges(games, false);

    if (games.empty())
        return "";

    // Clear any early key states (prevent instant selection on startup)
    for (int i = 0; i < 10; i++) {
        g_hal.update();
        delay(10);
    }

    // Pre-cache display and author strings to prevent heap fragmentation during rendering
    std::vector<GameItem> game_items;
    game_items.reserve(games.size());
    for (const auto& path : games)
    {
        GameItem item;
        item.path = path;

        size_t last_slash = path.find_last_of('/');
        std::string filename = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;

        std::string list_name = filename;
        if (list_name.length() > 18) {
            list_name = list_name.substr(0, 15) + "...";
        }
        item.list_name = list_name;

        // Uppercase filename
        std::string upper_name = filename;
        for (char &c : upper_name) c = toupper((unsigned char)c);
        item.right_name = upper_name.substr(0, 7);

        // Get author name
        std::string author = "UNKNOWN";
        if (upper_name.find("JELPI") != std::string::npos) author = "ZEP";
        else if (upper_name.find("CUTEBUNNIES") != std::string::npos) author = "THISISMYPASSPORT";
        else if (upper_name.find("LOOTSLIME") != std::string::npos) author = "KRAJZEG";
        else if (upper_name.find("PEGBALL") != std::string::npos) author = "MAXOSIRUS";
        else if (upper_name.find("31991") != std::string::npos) author = "TOM WRIGHT";

        for (char &c : author) c = toupper((unsigned char)c);
        item.right_author = author.substr(0, 7);

        game_items.push_back(item);
    }

    int selected = 0;
    int scroll_offset = 0;
    constexpr int VISIBLE_LINES = 5; // number of game names shown in 128x128 space

    // ── Draw background and PICO-8 styled border decorations once ──────────
    M5.Display.fillScreen(0x194A);

    // Left border area (X: 0..55)
    M5.Display.setTextColor(0xFBAF); // PICO-8 orange (#9)
    M5.Display.setTextSize(1);
    M5.Display.drawString("ZPT8PC", 4, 8, &fonts::Font0);
    M5.Display.setTextColor(0xFC60); // PICO-8 light gray (#6)
    M5.Display.drawString("LAYER8", 4, 24, &fonts::Font0);
    M5.Display.drawString("HW:S3", 4, 42, &fonts::Font0);
    M5.Display.drawString("RAM:32K", 4, 54, &fonts::Font0);
    M5.Display.drawString("SCR:128", 4, 66, &fonts::Font0);
    M5.Display.drawString("x128", 4, 78, &fonts::Font0);
    // Action key helper
    M5.Display.setTextColor(0xFBAF);
    M5.Display.drawString("BTN:", 4, 92, &fonts::Font0);
    M5.Display.setTextColor(0xFC60);
    M5.Display.drawString("Z/ENT:O", 4, 104, &fonts::Font0);
    M5.Display.drawString("X/SPC:X", 4, 116, &fonts::Font0);

    // Right border area (X: 184..239)
    // Dynamic names drawn at Y=8, Y=20 in draw_menu()
    M5.Display.setTextColor(0x6324); // PICO-8 pink (#14)
    M5.Display.drawString("SD:", 188, 36, &fonts::Font0);
    if (g_hal.hasSD()) {
        M5.Display.setTextColor(0x6BE0); // green
        M5.Display.drawString("OK", 210, 36, &fonts::Font0);
    } else {
        M5.Display.setTextColor(0xF400); // red
        M5.Display.drawString("NO", 210, 36, &fonts::Font0);
    }
    M5.Display.setTextColor(0xFC60); // PICO-8 light gray (#6)
    M5.Display.drawString("M5STACK", 188, 52, &fonts::Font0);
    M5.Display.drawString("CARDP", 188, 64, &fonts::Font0);
    // Cursor helper
    M5.Display.setTextColor(0x07FF); // cyan
    M5.Display.drawString("DIR:", 188, 86, &fonts::Font0);
    M5.Display.setTextColor(0xFC60);
    M5.Display.drawString(",./;", 188, 98, &fonts::Font0);
    M5.Display.drawString("WASD", 188, 110, &fonts::Font0);

    auto draw_menu = [&]() {
        // Clear only the 128x128 PICO-8 screen area (X: 56..183, Y: 3..130)
        M5.Display.fillRect(56, 3, 128, 128, TFT_BLACK);

        // ── Draw dynamic info on the right border area (X: 184..239) ──
        // Clear name and author area (Y: 8..31)
        M5.Display.fillRect(184, 8, 56, 24, 0x194A);

        if (selected >= 0 && selected < (int)game_items.size())
        {
            const auto& item = game_items[selected];
            M5.Display.setTextColor(0x07FF, 0x194A); // PICO-8 cyan (#12)
            M5.Display.drawString(item.right_name.c_str(), 188, 8, &fonts::Font0);
            M5.Display.setTextColor(0xFC60, 0x194A); // PICO-8 light gray (#6)
            M5.Display.drawString(item.right_author.c_str(), 188, 20, &fonts::Font0);
        }

        // Title bar (X: 56..183, Y: 3..15, height 12)
        M5.Display.fillRect(56, 3, 128, 12, TFT_NAVY);
        M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
        M5.Display.setTextSize(1);
        M5.Display.drawCenterString("Select Game", 120, 5, &fonts::Font0);

        // Game list (start at y=18, each item is 15px tall)
        M5.Display.setTextSize(1);
        for (int i = 0; i < VISIBLE_LINES && (scroll_offset + i) < (int)game_items.size(); i++)
        {
            int idx = scroll_offset + i;
            int y = 18 + i * 16;

            if (idx == selected)
            {
                // Highlight selected
                M5.Display.fillRect(56, y, 128, 14, TFT_DARKCYAN);
                M5.Display.setTextColor(TFT_YELLOW, TFT_DARKCYAN);
                M5.Display.drawString(">", 60, y, &fonts::Font0);
            }
            else
            {
                M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
            }

            const auto& item = game_items[idx];
            M5.Display.drawString(item.list_name.c_str(), (idx == selected) ? 70 : 62, y, &fonts::Font0);
        }

        // Footer inside 128x128 area (Y: 114)
        M5.Display.setTextColor(TFT_GRAY, TFT_BLACK);
        M5.Display.drawCenterString("Enter/Space:Load", 120, 114, &fonts::Font0);
    };

    draw_menu();

    while (true)
    {
        g_hal.update();

        // Navigate up/down
        if (g_hal.getButton(0, 2)) // up
        {
            if (selected > 0) {
                selected--;
                if (selected < scroll_offset)
                    scroll_offset = selected;
                draw_menu();
            }
            while (g_hal.getButton(0, 2)) { g_hal.update(); delay(10); }
        }
        if (g_hal.getButton(0, 3)) // down
        {
            if (selected < (int)game_items.size() - 1) {
                selected++;
                if (selected >= scroll_offset + VISIBLE_LINES)
                    scroll_offset = selected - VISIBLE_LINES + 1;
                draw_menu();
            }
            while (g_hal.getButton(0, 3)) { g_hal.update(); delay(10); }
        }

        // Load selected game (O button = index 4 or X button = index 5)
        if (g_hal.getButton(0, 4) || g_hal.getButton(0, 5))
        {
            while (g_hal.getButton(0, 4) || g_hal.getButton(0, 5)) { g_hal.update(); delay(10); }
            delay(200); // Prevent instant selection carryover
            return game_items[selected].path;
        }

        // Cancel (X button = index 5)
        if (g_hal.getButton(0, 5))
        {
            while (g_hal.getButton(0, 5)) { g_hal.update(); delay(10); }
            return "";
        }

        delay(20);
    }
}

void setup()
{
    Serial.begin(115200);
    // Wait up to 3 seconds for USB serial connection
    unsigned long start_usb = millis();
    while (!Serial && (millis() - start_usb) < 3000)
    {
        delay(10);
    }
    Serial.println("Zepto-8PC v0.3 Starting...");

    // 1. Init SD card FIRST so we can load the cartridges
    if (!g_hal.initSD())
    {
        Serial.println("SD card init failed, but trying to continue...");
    }

    // 2. SPIFFSをマウント (bios.p8読み込み用) - Keep mounted for soft-reboots
#if defined(ARDUINO)
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed!");
    }
#endif

    // VM is dynamically allocated to save static RAM space
    g_vm = new z8::pico8::vm();

    // 5. Initalize HAL (displays, sound, keyboard, battery)
    if (!g_hal.begin())
    {
        Serial.println("HAL initialization failed!");
        while (1) delay(100);
    }

    // 6. Show splash screen then always go to game selector
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.drawCenterString("Zepto-8PC", 120, 30);
    M5.Display.setTextSize(1);
    M5.Display.drawCenterString("v0.3", 120, 55);

    M5.Display.drawCenterString("Loading...", 120, 100);
    delay(1500);

    // Always show game selector on startup
    Serial.println("Entering game selector...");
    std::string cart_path = show_game_selector();
    if (!cart_path.empty())
    {
        if (cart_path == "__CONSOLE__")
        {
            // Load BIOS (console)
            Serial.println("Loading Zepto-8 Console (bios.p8)...");
            g_vm->load("/bios.p8");
#if defined(ARDUINO)
            SPIFFS.end();
            Serial.println("SPIFFS unmounted after BIOS load.");
#endif
            g_vm->run();
        }
        else
        {
            Serial.println("[DEBUG] Recreating VM to clear RAM before game load...");
            delete g_vm;
#if defined(ARDUINO)
            SPIFFS.end();
            Serial.println("SPIFFS unmounted before Game load.");
#endif
            g_vm = new z8::pico8::vm();

            Serial.printf("Loading selected cartridge: %s\n", cart_path.c_str());
            Serial.println("System initializing...");
            std::string path = cart_path;
            Serial.printf("Loading cartridge: %s\n", path.c_str());
            g_vm->load(path.c_str());
            Serial.println("Cartridge loaded successfully. Starting VM run...");
            g_vm->run();
            Serial.println("VM run finished (initialization complete).");
        }

        // If selected cartridge failed to run, show selector again
        if (!g_vm->is_running()) {
            Serial.println("Cartridge failed to run, showing selector again...");
            g_vm->reset();
            delay(500);
            cart_path = show_game_selector();
            if (!cart_path.empty()) {
                if (cart_path == "__CONSOLE__")
                {
#if defined(ARDUINO)
                    SPIFFS.begin(true);
#endif
                    Serial.println("Loading Zepto-8 Console (bios.p8)...");
                    g_vm->load("/bios.p8");
#if defined(ARDUINO)
                    SPIFFS.end();
#endif
                    g_vm->run();
                }
                else
                {
                    Serial.printf("Loading selected cartridge: %s\n", cart_path.c_str());
                    g_vm->load(cart_path);
                    g_vm->run();
                }
            }
        }
    }

    // Create audio sync semaphore
    g_audio_sem = xSemaphoreCreateBinary();

    // Launch audio synthesis task on Core0 (main loop runs on Core1)
    xTaskCreatePinnedToCore(
        audioTask,          // task function
        "AudioTask",        // name
        2048,               // stack (bytes) (Restored to 2048 to prevent stack overflow)
        nullptr,            // arg
        1,                  // priority
        &g_audio_task_handle,
        0                   // Core0
    );
    Serial.println("Audio task started on Core0.");
}
void loop()
{
    unsigned long start_time = millis();

    if (g_vm && g_vm->is_running())
    {
        static uint32_t last_frame_time = millis();
        uint32_t now = millis();
        uint32_t delta = now - last_frame_time;
        last_frame_time = now;

        auto [ram, size] = g_vm->ram();
        if (ram && size > 0x5f80) {
            ram[0x5f80] = (delta > 40) ? 1 : 0;
        }

        // static uint32_t frame_count = 0;
        // if ((frame_count++ % 100) == 0) {
        //     Serial.printf("Loop running, delta=%d\n", delta);
        // }

        // 1. Update hardware inputs
        g_hal.update();

        // 2. Map buttons to PICO-8 VM
        for (int player = 0; player < 2; ++player)
        {
            for (int btn = 0; btn < 8; ++btn)
            {
                if (g_hal.getButton(player, btn))
                {
                    g_vm->button(player, btn, 1);
                }
            }
        }

        // 3. Map keyboard text to PICO-8 VM
        while (g_hal.hasText())
        {
            char c = g_hal.getChar();
            g_vm->text(c);
        }

        // 4. Step VM
        g_vm->step(1.0f / 30.0f);

        // 5. Render Screen — render_fast writes directly to HAL's RGB565 buffer
        uint16_t* target_screen_buf = g_hal.getScreenBuffer();
        g_vm->render_fast(target_screen_buf);
        g_hal.pushScreenBuffer();

        // 6. Audio synthesis
        if (g_audio_sem) {
            g_vm->get_audio(g_audio_dbuf[g_audio_write_idx], AUDIO_SAMPLES * sizeof(uint8_t));
            g_audio_write_idx = 1 - g_audio_write_idx;
            xSemaphoreGive(g_audio_sem);
        }

    }
    else if (g_vm)
    {
        // Error & restart handling
        static bool error_shown = false;
        if (!error_shown) {
            error_shown = true;
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.fillRect(40, 50, 160, 35, TFT_RED);
            M5.Display.setTextColor(TFT_WHITE);
            M5.Display.setTextSize(1);
            M5.Display.drawCenterString("VM ERROR", 120, 60, &fonts::Font2);
        }

        static unsigned long last_reboot = 0;
        if (millis() - last_reboot > 3000) {
            last_reboot = millis();
            Serial.println("[DEBUG] Recreating VM to clear RAM on error reboot...");
            delete g_vm;
            g_vm = new z8::pico8::vm();
            delay(100);
            std::string cart_path = find_cartridge();
            if (cart_path.empty()) {
                cart_path = "/spiffs/jelpi.p8";
            }
            if (!cart_path.empty()) {
                g_vm->load(cart_path);
            }
            g_vm->run();
            error_shown = false;
        }
        delay(50);
    }

    unsigned long elapsed = millis() - start_time;
    if (elapsed < 33)
    {
        delay(33 - elapsed);
    }
}
