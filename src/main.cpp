#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

#include "cmt_cmt_converter.h"
#include "cmt_control.h"
#include "cmt_core1.h"
#include "cmt_globals.h"
#include "cmt_hw_config.h"
#include "cmt_network.h"
#include "cmt_pins.h"
#include "cmt_storage.h"
#include "cmt_storage_record.h"
#include "cmt_t88_converter.h"
#include "cmt_types.h"
#include "cmt_ui.h"
#include "cmt_ui_input.h"
#include "hardware/watchdog.h"
#include "pico/util/queue.h"

volatile SystemState current_state = STATE_IDLE;
RecMode selected_rec_mode = MODE_NEW;
RemoteState current_remote_state = REMOTE_OFF;
uint32_t remote_stable_time = 0;

volatile bool playback_permitted = false;
volatile bool recording_permitted = false;
bool manual_ok_override = false;
volatile bool playback_user_paused = false;

volatile bool recording_active = false;
volatile bool play_active = false;
volatile bool rec_paused = true;
volatile bool play_paused = true;

volatile bool playback_started = false;

FileItem file_list[MAX_FILES];
int total_items = 0;
int current_cursor = 0;
int scroll_offset = 0;
String current_path = "/";

queue_t cmt_data_queue;
queue_t cmt_play_queue;
int buffer_idx = 0;

File cmt_file;
int rec_menu_cursor = 0;
char target_filename[128];

ButtonTracker buttons[NUM_BUTTONS];
volatile bool btn_trigger_clicked[NUM_BUTTONS] = {false};
volatile bool btn_trigger_repeat[NUM_BUTTONS] = {false};

Adafruit_SSD1306 oled(128, 64, &Wire1, -1);

int pio_rec_sm = 0;
int pio_play_sm = 1;

bool oled_need_refresh = true;
bool sd_card_inserted = false;
bool sd_card_mounted = false;
static uint32_t last_idle_oled_refresh_ms = 0;
static uint32_t last_playback_oled_refresh_ms = 0;
static uint32_t stop_button_hold_start_ms = 0;

int cmt_selected_baud_rate = 1200;
int cas_selected_baud_rate = 1200;

static bool is_sd_inserted() {
    static uint32_t last_probe_ms = 0;
    static bool last_probe_result = false;
    uint32_t now = millis();

    if (now - last_probe_ms < 1000) {
        return last_probe_result;
    }
    last_probe_ms = now;

    if (sd_card_mounted) {
        File root = SD.open("/");
        if (root) {
            root.close();
            last_probe_result = true;
            return true;
        }
        last_probe_result = false;
        return false;
    } else {
        if (gpio_get(PICO_SPI_DETECT_PIN) == 0) {
            last_probe_result = true;
            return true;
        }
        SD.end();
        if (SD.begin(PICO_SD_CS_PIN, SD_SCK_MHZ(24), SPI1)) {
            SD.end();
            last_probe_result = true;
            return true;
        }
        last_probe_result = false;
        return false;
    }
}

static bool mount_sd_card() {
    SD.end();
    if (SD.begin(PICO_SD_CS_PIN, SD_SCK_MHZ(16), SPI1)) {
        sd_card_mounted = true;
        sd_card_inserted = true;
        if (!load_directory(current_path)) {
            SD.end();
            sd_card_mounted = false;
            sd_card_inserted = false;
            return false;
        }
        return true;
    }
    sd_card_mounted = false;
    return false;
}

static void refresh_directory_if_idle() {
    static uint32_t last_refresh_ms = 0;
    uint32_t now_ms = millis();

    if (current_state != STATE_IDLE || !sd_card_mounted) {
        return;
    }
    if ((now_ms - last_refresh_ms) < 2000) {
        return;
    }
    last_refresh_ms = now_ms;

    int prev_total = total_items;
    int prev_cursor = current_cursor;
    int prev_scroll = scroll_offset;

    char selected_name[MAX_NAME_LEN] = {0};
    bool selected_is_dir = false;
    bool has_selection = false;

    if (total_items > 0 && current_cursor >= 0 &&
        current_cursor < total_items) {
        snprintf(selected_name, sizeof(selected_name), "%s",
                 file_list[current_cursor].name);
        selected_is_dir = file_list[current_cursor].is_dir;
        has_selection = true;
    }

    if (!load_directory(current_path)) {
        SD.end();
        sd_card_mounted = false;
        sd_card_inserted = false;
        total_items = 0;
        current_cursor = 0;
        scroll_offset = 0;
        oled_need_refresh = true;
        return;
    }

    if (has_selection && total_items > 0) {
        int restored_idx = -1;
        for (int i = 0; i < total_items; i++) {
            if (file_list[i].is_dir == selected_is_dir &&
                strcmp(file_list[i].name, selected_name) == 0) {
                restored_idx = i;
                break;
            }
        }
        if (restored_idx >= 0) {
            current_cursor = restored_idx;
        } else if (prev_cursor < total_items) {
            current_cursor = (prev_cursor >= 0) ? prev_cursor : 0;
        } else {
            current_cursor = (total_items > 0) ? (total_items - 1) : 0;
        }
    }

    if (total_items <= 0) {
        current_cursor = 0;
        scroll_offset = 0;
    } else {
        if (current_cursor < 0) {
            current_cursor = 0;
        }
        if (current_cursor >= total_items) {
            current_cursor = total_items - 1;
        }

        scroll_offset = prev_scroll;
        if (current_cursor < scroll_offset) {
            scroll_offset = current_cursor;
        }
        if (current_cursor - scroll_offset >= 4) {
            scroll_offset = current_cursor - 3;
        }
        if (total_items > 4 && scroll_offset > total_items - 4) {
            scroll_offset = total_items - 4;
        }
        if (scroll_offset < 0) {
            scroll_offset = 0;
        }
    }

    if (total_items != prev_total || current_cursor != prev_cursor ||
        scroll_offset != prev_scroll) {
        oled_need_refresh = true;
    }
}

static void monitor_sd_card_hotplug() {
    uint32_t now = millis();
    static uint32_t last_check_ms = 0;

    if (now - last_check_ms < 500) {
        return;
    }
    last_check_ms = now;

    bool detect_pin_inserted = (gpio_get(PICO_SPI_DETECT_PIN) == 0);

    if (sd_card_mounted) {
        if (!detect_pin_inserted) {
            SD.end();
            sd_card_mounted = false;
            sd_card_inserted = false;
            recording_active = false;
            rec_paused = true;
            stop_playback_and_return_idle();

            current_state = STATE_IDLE;
            current_path = "/";
            total_items = 0;
            current_cursor = 0;
            scroll_offset = 0;
            reset_and_ignore_pressed_buttons();
            oled_need_refresh = true;
        }
    } else {
        if (detect_pin_inserted) {
            if (mount_sd_card()) {
                current_state = STATE_IDLE;
                reset_and_ignore_pressed_buttons();
                oled_need_refresh = true;
            }
        } else {
            static uint32_t last_probe_ms = 0;
            if (now - last_probe_ms > 2000) {
                last_probe_ms = now;
                if (mount_sd_card()) {
                    current_state = STATE_IDLE;
                    reset_and_ignore_pressed_buttons();
                    oled_need_refresh = true;
                }
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 3000)) {
        delay(100);
    }
    Serial.println("\n--- CMT to T88 Converter Starting ---");

    init_ui_buttons();

    gpio_init(REMOTE_PIN);
    gpio_set_dir(REMOTE_PIN, GPIO_IN);
    gpio_pull_up(REMOTE_PIN);

    gpio_init(PICO_SPI_DETECT_PIN);
    gpio_set_dir(PICO_SPI_DETECT_PIN, GPIO_IN);
    gpio_pull_up(PICO_SPI_DETECT_PIN);

    init_cores_communication();
    watchdog_enable(4000, 0);

    Wire1.setSDA(PICO_I2C_SDA_PIN);
    Wire1.setSCL(PICO_I2C_SCL_PIN);
    Wire1.begin();

    if (!oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        while (1) {
            delay(100);
        }
    }

    init_wifi_and_settings();

    SPI1.setRX(PICO_SPI_MISO_PIN);
    SPI1.setTX(PICO_SPI_MOSI_PIN);
    SPI1.setSCK(PICO_SPI_CLK_PIN);
    SPI1.begin();
    pinMode(PICO_SD_CS_PIN, OUTPUT);
    digitalWrite(PICO_SD_CS_PIN, HIGH);

    mount_sd_card();

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.println("PicoCmt");
    oled.setCursor(0, 12);
    oled.println("WiFi: OFF");
    oled.display();
    oled_need_refresh = true;
}

void loop() {
    watchdog_update();
    uint32_t now_ms = millis();

    bool stop_pressed_raw = (gpio_get(BTN_STOP_PIN) == 0);
    if (stop_pressed_raw) {
        if (stop_button_hold_start_ms == 0) {
            stop_button_hold_start_ms = now_ms;
        } else if ((now_ms - stop_button_hold_start_ms) > 800) {
            if (current_state == STATE_PLAYING ||
                current_state == STATE_PLAY_STANDBY) {
                stop_playback_and_return_idle();
                reset_and_ignore_pressed_buttons();
                oled_need_refresh = true;
            }
            stop_button_hold_start_ms = now_ms;
        }
    } else {
        stop_button_hold_start_ms = 0;
    }

    handle_wifi_background_tasks();

    if (current_state == STATE_PLAY_STANDBY || current_state == STATE_PLAYING) {
        if ((now_ms - last_playback_oled_refresh_ms) >= 250) {
            last_playback_oled_refresh_ms = now_ms;
            oled_need_refresh = true;
        }
    } else {
        last_playback_oled_refresh_ms = 0;
    }

    static uint32_t last_rec_oled_refresh_ms = 0;
    if (current_state == STATE_RECORDING) {
        if ((now_ms - last_rec_oled_refresh_ms) >= 250) {
            last_rec_oled_refresh_ms = now_ms;
            oled_need_refresh = true;
        }
    } else {
        last_rec_oled_refresh_ms = 0;
    }

    if (current_state == STATE_IDLE && sd_card_mounted) {
        if ((now_ms - last_idle_oled_refresh_ms) >= 180) {
            last_idle_oled_refresh_ms = now_ms;
            oled_need_refresh = true;
        }
    } else {
        last_idle_oled_refresh_ms = 0;
    }

    scan_ui_buttons();
    scan_remote_hardware();
    monitor_sd_card_hotplug();
    refresh_directory_if_idle();

    switch (current_state) {
        case STATE_IDLE:
            if (sd_card_mounted) {
                handle_file_browser_input();
            }
            break;
        case STATE_REC_MENU:
            handle_rec_menu_input();
            break;
        case STATE_WIFI_MENU:
            handle_wifi_menu_input();
            break;
        case STATE_WIFI_CONFIG:
            handle_wifi_config_input();
            break;
        case STATE_WIFI_PIN_CONNECT:
            handle_wifi_pin_connect_input();
            break;
        case STATE_WIFI_SAVE_RESULT:
            if ((int32_t)(now_ms - wifi_save_result_until_ms) >= 0) {
                wifi_menu_cursor = wifi_settings_saved ? 0 : 1;
                current_state = STATE_WIFI_MENU;
                reset_and_ignore_pressed_buttons();
                oled_need_refresh = true;
            }
            break;
        case STATE_WIFI_CONNECT_ERROR:
            handle_wifi_connect_error_input();
            break;
        case STATE_REC_STANDBY:
        case STATE_PLAY_STANDBY:
            handle_ui_buttons_in_standby();
            break;
        case STATE_RECORDING:
            handle_raw_recording_stream();

            if (!recording_active || btn_trigger_clicked[IDX_STOP]) {
                if (btn_trigger_clicked[IDX_STOP]) {
                    btn_trigger_clicked[IDX_STOP] = false;
                    stop_raw_recording_and_finalize();
                }

                if (default_rec_fmt == FMT_T88) {
                    Serial.println(
                        "Starting conversion: /input.raw -> /output.t88");
                    convert_raw_to_t88(raw_rec_path, target_filename);
                } else if (default_rec_fmt == FMT_CMT) {
                    Serial.println(
                        "Starting conversion: /input.raw -> /output.cmt");
                    convert_raw_to_cmt(raw_rec_path, target_filename);
                } else {
                    Serial.println(
                        "Warning: Conversion for this format is not "
                        "implemented yet.");
                }

                current_state = STATE_IDLE;
                reset_and_ignore_pressed_buttons();
                oled_need_refresh = true;
            }
            break;
        case STATE_PLAYING:
            handle_playback_active_inputs();

            if (btn_trigger_clicked[IDX_PLAY]) {
                btn_trigger_clicked[IDX_PLAY] = false;
                play_paused = true;
                playback_user_paused = true;
                playback_permitted = false;
                current_state = STATE_PLAY_STANDBY;
                reset_and_ignore_pressed_buttons();
                oled_need_refresh = true;
                break;
            }

            if (btn_trigger_clicked[IDX_STOP] || btn_trigger_repeat[IDX_STOP]) {
                btn_trigger_clicked[IDX_STOP] = false;
                btn_trigger_repeat[IDX_STOP] = false;
                stop_playback_and_return_idle();
                reset_and_ignore_pressed_buttons();
                oled_need_refresh = true;
                break;
            }

            handle_sd_playback_stream();
            monitor_playback_finalize();
            break;
    }

    update_oled_display();
    delay(1);
}