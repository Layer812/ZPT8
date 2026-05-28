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
// 11025Hz: 1 frame (30fps) = 368 samples  (halved from 22050Hz to save CPU)
static constexpr int AUDIO_SAMPLES = 368;
static constexpr int AUDIO_RATE    = 11025;
static int16_t g_audio_dbuf[2][AUDIO_SAMPLES];
static volatile int  g_audio_write_idx = 0;   // index main loop writes to
static volatile bool g_audio_buf_ready = false; // new buffer waiting to be queued
static SemaphoreHandle_t g_audio_sem = nullptr;
static TaskHandle_t g_audio_task_handle = nullptr;

// Audio task: runs on Core0, queues completed buffers to speaker
void audioTask(void* /*arg*/)
{
    while (true)
    {
        // Wait for signal from main loop (new buffer ready)
        if (xSemaphoreTake(g_audio_sem, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            int read_idx = 1 - g_audio_write_idx; // opposite side
            g_hal.queueAudio(g_audio_dbuf[read_idx], AUDIO_SAMPLES);
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
                (name.length() >= 4 && name.substr(name.length() - 4) == ".p8c"))
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
                    (name.length() >= 4 && name.substr(name.length() - 4) == ".p8c"))
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
                (name.length() >= 4 && name.substr(name.length() - 4) == ".p8c"))
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
                (name.length() >= 4 && name.substr(name.length() - 4) == ".p8c"))
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

    // Right border area (X: 184..239)
    M5.Display.setTextColor(0x07FF); // PICO-8 cyan (#12)
    M5.Display.drawString("40 FPS", 188, 8, &fonts::Font0);
    M5.Display.setTextColor(0xFC60); // PICO-8 light gray (#6)
    M5.Display.drawString("M5STACK", 188, 26, &fonts::Font0);
    M5.Display.drawString("CARD", 188, 38, &fonts::Font0);
    M5.Display.drawString("PUTER", 188, 50, &fonts::Font0);

    auto draw_menu = [&]() {
        // Clear only the 128x128 PICO-8 screen area (X: 56..183, Y: 3..130)
        M5.Display.fillRect(56, 3, 128, 128, TFT_BLACK);

        // Title bar (X: 56..183, Y: 3..15, height 12)
        M5.Display.fillRect(56, 3, 128, 12, TFT_NAVY);
        M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
        M5.Display.setTextSize(1);
        M5.Display.drawCenterString("Select Game", 120, 5, &fonts::Font0);

        // Game list (start at y=18, each item is 15px tall)
        M5.Display.setTextSize(1);
        for (int i = 0; i < VISIBLE_LINES && (scroll_offset + i) < (int)games.size(); i++)
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

            // Extract just the filename from the path
            std::string display_name = games[idx];
            size_t pos = display_name.find_last_of('/');
            if (pos != std::string::npos)
                display_name = display_name.substr(pos + 1);

            // Remove extension (.pc8c or .p8c)
            size_t dot_pos = display_name.find_last_of('.');
            if (dot_pos != std::string::npos)
                display_name = display_name.substr(0, dot_pos);

            // Truncate to fit the 128px width space (approx 16-18 chars for Font0)
            if (display_name.length() > 18)
                display_name = display_name.substr(0, 15) + "...";

            M5.Display.drawString(display_name.c_str(), (idx == selected) ? 70 : 62, y, &fonts::Font0);
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
            if (selected < (int)games.size() - 1) {
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
            return games[selected];
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

int dump_writer(lua_State *L, const void *p, size_t sz, void *ud) {
    for (size_t i = 0; i < sz; i++) {
        Serial.printf("0x%02X, ", ((const uint8_t*)p)[i]);
    }
    return 0;
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
    Serial.println("Zepto-8PC v0.2 Starting...");

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
    M5.Display.drawCenterString("v0.2", 120, 55);

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
            g_vm->run();
        }
        else
        {
            Serial.printf("Loading selected cartridge: %s\n", cart_path.c_str());
            g_vm->load(cart_path);
            g_vm->run();
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
                    Serial.println("Loading Zepto-8 Console (bios.p8)...");
                    g_vm->load("/bios.p8");
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

    // Launch audio synthesis task on Core0 (main loop runs on Core1)
    g_audio_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(
        audioTask,          // task function
        "AudioTask",        // name
        2048,               // stack (bytes)
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
    static lol::u8vec4* temp_rgba_buf = nullptr;
    static unsigned long last_heartbeat = 0;
    static bool printed_info = false;

    static unsigned long t_update_sum = 0;
    static unsigned long t_step_sum = 0;
    static unsigned long t_render_sum = 0;
    static unsigned long t_disp_sum = 0;
    static unsigned long t_synth_sum = 0;
    static unsigned long t_queue_sum = 0;
    static unsigned long t_total_sum = 0;
    static int frame_count = 0;

    if (millis() - last_heartbeat > 5000)
    {
        last_heartbeat = millis();
        Serial.printf("[HEARTBEAT] VM is running. Free heap: %u bytes\n", (unsigned int)ESP.getFreeHeap());
    }

    if (!printed_info && millis() > 10000) 
    {
        printed_info = true;
        Serial.println("====== SYSTEM DEBUG INFO ======");
        if (SD.exists("/bios.p8")) {
            File f = SD.open("/bios.p8", FILE_READ);
            Serial.printf("SD: /bios.p8 exists, size: %d bytes\n", (int)f.size());
            f.close();
        }
        if (SD.exists("/jelpi.p8")) {
            File f = SD.open("/jelpi.p8", FILE_READ);
            Serial.printf("SD: /jelpi.p8 exists, size: %d bytes\n", (int)f.size());
            f.close();
        }
        Serial.println("===============================");
    }

    if (g_vm && g_vm->is_running())
    {
        unsigned long t0 = micros();

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

        unsigned long t1 = micros();

        // 4. Step VM
        g_vm->step(1.0f / 30.0f);

        unsigned long t2 = micros();

        // 5. Render Screen (高速・通常のどちらでも100%液晶を光らせる無敵ルート)
        unsigned long t3;
        
        // HALが持っている標準バッファ（RGB565用）のアドレスを取得
        uint16_t* target_screen_buf = g_hal.getScreenBuffer();

        if (g_vm->render_fast(target_screen_buf))
        {
            // コアが直で高速描画してくれた場合は、そのまま転送！
            t3 = micros();
            g_hal.pushScreenBuffer(); 
        }
        else
        {
            // ⭕ 【ここがエルドラド救済の核心】
            // render_fastが弾かれた場合、一度一時的なRGBAバッファに描画させ、
            // それを HAL 側の正しい RGB565バッファへと安全にデコードして流し込みます。
            if (!temp_rgba_buf) {
                temp_rgba_buf = (lol::u8vec4*)malloc(128 * 128 * sizeof(lol::u8vec4));
                Serial.printf("[DEBUG] Allocated temp_rgba_buf: %p (64KB), free heap: %u\n", temp_rgba_buf, (unsigned)ESP.getFreeHeap());
            }
            if (temp_rgba_buf) {
                g_vm->render(temp_rgba_buf);
            }
            t3 = micros();

            // RGBA を RGB565 (Big Endian) に変換して HAL の画面バッファへ転写
            for (int i = 0; i < 128 * 128; i++) {
                uint8_t r = temp_rgba_buf[i].r;
                uint8_t g = temp_rgba_buf[i].g;
                uint8_t b = temp_rgba_buf[i].b;
                uint16_t rgb = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                target_screen_buf[i] = (rgb >> 8) | (rgb << 8); // バイトスワップ
            }

            // 128x128バッファが完全に仕上がったので、液晶に強制プッシュ！
            g_hal.pushScreenBuffer(); 
        }

        unsigned long t4 = micros();

        // 6. Audio synthesis
        unsigned long t4_1 = micros();
        g_vm->get_audio(g_audio_dbuf[g_audio_write_idx], AUDIO_SAMPLES * sizeof(int16_t));
        unsigned long t4_2 = micros();

        g_audio_write_idx = 1 - g_audio_write_idx;
        xSemaphoreGive(g_audio_sem);

        unsigned long t5 = micros();

        t_update_sum += (t1 - t0);
        t_step_sum += (t2 - t1);
        t_render_sum += (t3 - t2);
        t_disp_sum += (t4 - t3);
        t_synth_sum += (t4_2 - t4_1);
        t_queue_sum += (t5 - t4_2);
        t_total_sum += (t5 - t0);
        frame_count++;

        if (frame_count >= 100)
        {
            Serial.printf("[PROFILE] 100 frames avg (us): Update=%lu, VMStep=%lu, RenderVM=%lu, DispPush=%lu, AudioSynth=%lu, AudioQueue=%lu, TotalLoop=%lu\n",
                          t_update_sum / 100, t_step_sum / 100, t_render_sum / 100, t_disp_sum / 100, t_synth_sum / 100, t_queue_sum / 100, t_total_sum / 100);
            t_update_sum = t_step_sum = t_render_sum = t_disp_sum = t_synth_sum = t_queue_sum = t_total_sum = 0;
            frame_count = 0;
        }
    }
    else if (g_vm)
    {
        // エラー＆再起動処理（既存のまま完全維持）
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
            g_vm->reset();
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

    if (g_vm && !g_vm->is_running())
    {
        if (temp_rgba_buf)
        {
            free(temp_rgba_buf);
            temp_rgba_buf = nullptr;
            Serial.printf("[DEBUG] Freed temp_rgba_buf (64KB), free heap: %u\n", (unsigned)ESP.getFreeHeap());
        }
    }

    unsigned long elapsed = millis() - start_time;
    if (elapsed < 33)
    {
        delay(33 - elapsed);
    }
}