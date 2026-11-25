#include "landing_screen.h"
#include "ui.h"

#include "eez-flow.h"
#include "screens.h"
#include "sd_service.h"
#include "lora_service.h"
#include "gps_service.h"
#include "ina_service.h"
#include "temp_service.h"
#include "board_config.h"
#include "audio_service.h"
#include "wifi_service.h"

#include <cstring>
#include <cstdlib>

namespace
{
const lv_color_t Background = lv_color_hex(0x081029);
const lv_color_t Card = lv_color_hex(0x102442);
const lv_color_t Text = lv_color_hex(0xF7F1EC);
const lv_color_t Muted = lv_color_hex(0x8C9CA5);
const lv_color_t Accent = lv_color_hex(0x42F7AD);
const char *keyboards[] = {
    "1234567890qwertyuiopasdfghjkl;zxcvbnm,./@_-+=!?()[]{}",
    "1234567890QWERTYUIOPASDFGHJKL:ZXCVBNM<>?@_-=+!#$%^&*",
    "0123456789!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~abcdefghij"};

enum class Screen { Dashboard, Wifi };
enum class WifiAction {
    SelectSsid = 1,
    SelectPassword,
    Delete,
    Space,
    ChangeKeyboard,
    Connect,
    Close,
    Open,
    FirstKey = 100
};

Screen current_screen = Screen::Dashboard;
lv_obj_t *landing_screen;
lv_obj_t *wifi_screen;
lv_obj_t *ssid_label;
lv_obj_t *password_label;
lv_obj_t *wifi_feedback;
lv_obj_t *dashboard_status;
lv_obj_t *ssid_field;
lv_obj_t *password_field;
lv_obj_t *lora_status_label;
lv_obj_t *temperature_label;
lv_obj_t *temperature_status_label;
lv_obj_t *gps_status_label;
lv_obj_t *downlink_status_label;
lv_obj_t *energy_labels[INA_SENSOR_COUNT][3];
lv_obj_t *battery_percent_label;
lv_obj_t *key_labels[50];
char ssid[33] = {};
char password[65] = {};
char wifi_status[160] = "Starting WiFi...";
char sd_status[128] = "Starting SD card...";
char lora_status[160] = "Starting LoRaWAN...";
char gps_status[128] = "Waiting for GPS data...";
char audio_status[128] = "Initializing audio...";
char downlink_status[256] = "No downlink received";
bool password_selected = false;
uint8_t keyboard_page = 0;

void update_energy_labels(lv_obj_t *const (&labels)[3], const InaReading &reading)
{
    char text[32];
    if (!reading.connected) {
        lv_label_set_text(labels[0], "-- V");
        lv_label_set_text(labels[1], "-- A");
        lv_label_set_text(labels[2], "-- W");
        return;
    }

    snprintf(text, sizeof(text), "%.2f V", reading.voltage);
    lv_label_set_text(labels[0], text);
    if (reading.over_range) {
        // Short, so it fits next to the title in the narrower cards.
        lv_label_set_text(labels[1], "> MAX");
        lv_label_set_text(labels[2], "> MAX");
        return;
    }
    snprintf(text, sizeof(text), "%.3f A", reading.current);
    lv_label_set_text(labels[1], text);
    snprintf(text, sizeof(text), "%.2f W", reading.power);
    lv_label_set_text(labels[2], text);
}

const char *wifi_input_error()
{
    const size_t password_length = strlen(password);
    if (!ssid[0]) {
        return "Enter an SSID";
    }
    if (password_length == 0 || (password_length >= 8 && password_length <= 63) ||
        (password_length == 64 && strspn(password, "0123456789abcdefABCDEF") == 64)) {
        return nullptr;
    }
    return "Password: 8-63 characters or 64 hex digits; empty for open WiFi";
}

void update_wifi_fields()
{
    if (ssid_label) {
        lv_label_set_text(ssid_label, ssid);
    }
    if (password_label) {
        char mask[sizeof(password)] = {};
        memset(mask, '*', strlen(password));
        lv_label_set_text(password_label, mask);
    }
    if (wifi_feedback) {
        const char *error = wifi_input_error();
        lv_label_set_text(wifi_feedback, error ? error : wifi_status);
    }
    if (dashboard_status) {
        char status[sizeof(wifi_status) + sizeof(sd_status) + 4];
        snprintf(status, sizeof(status), "%s | %s", wifi_status, sd_status);
        lv_label_set_text(dashboard_status, status);
    }
    for (size_t i = 0; i < 50; ++i) {
        if (key_labels[i]) {
            char key[] = {keyboards[keyboard_page][i], '\0'};
            lv_label_set_text(key_labels[i], key);
        }
    }
}

void lora_uplink_button_event(lv_event_t *event)
{
    const LoraUplink uplink = static_cast<LoraUplink>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        !lora_service_request_uplink(uplink)) {
        strlcpy(lora_status, "LoRaWAN request queue full or unavailable",
                sizeof(lora_status));
        if (lora_status_label) {
            lv_label_set_text(lora_status_label, lora_status);
        }
    }
}

lv_obj_t *create_button(lv_obj_t *parent, int x, int y, int width, int height,
                        const char *text, bool accent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, accent ? Accent : Card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 10, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, accent ? Background : Text, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    return button;
}

lv_obj_t *create_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, Card, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    return card;
}

void add_label(lv_obj_t *parent, int x, int y, const char *text, lv_color_t color,
               const lv_font_t *font = LV_FONT_DEFAULT)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
}

void open_existing_screens(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        // The app starts on the energy overview, before choosing a learner.
        eez_flow_set_screen(SCREEN_ID_AI_FORECAST, LV_SCR_LOAD_ANIM_NONE, 0, 0);
    }
}

void update_field_selection()
{
    lv_obj_set_style_bg_color(ssid_field, password_selected ? Card : Accent, LV_PART_MAIN);
    lv_obj_set_style_bg_color(password_field, password_selected ? Accent : Card, LV_PART_MAIN);
    lv_obj_set_style_text_color(ssid_label, password_selected ? Text : Background, LV_PART_MAIN);
    lv_obj_set_style_text_color(password_label, password_selected ? Background : Text, LV_PART_MAIN);
}

void wifi_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        current_screen = Screen::Wifi;
        update_wifi_fields();
        update_field_selection();
        lv_screen_load(wifi_screen);
    }
}

void wifi_action_event(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const intptr_t action = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (action == static_cast<intptr_t>(WifiAction::SelectSsid)) {
        password_selected = false;
    } else if (action == static_cast<intptr_t>(WifiAction::SelectPassword)) {
        password_selected = true;
    } else if (action == static_cast<intptr_t>(WifiAction::Delete)) {
        char *field = password_selected ? password : ssid;
        const size_t length = strlen(field);
        if (length > 0) {
            field[length - 1] = '\0';
        }
    } else if (action == static_cast<intptr_t>(WifiAction::Space)) {
        char *field = password_selected ? password : ssid;
        const size_t capacity = password_selected ? sizeof(password) : sizeof(ssid);
        const size_t length = strlen(field);
        if (length + 1 < capacity) {
            field[length] = ' ';
            field[length + 1] = '\0';
        }
    } else if (action == static_cast<intptr_t>(WifiAction::ChangeKeyboard)) {
        keyboard_page = (keyboard_page + 1) % 3;
    } else if (action == static_cast<intptr_t>(WifiAction::Connect)) {
        if (!wifi_input_error()) {
            if (wifi_submit_credentials(ssid, password)) {
                memset(password, 0, sizeof(password));
                current_screen = Screen::Dashboard;
                lv_screen_load(landing_screen);
            } else {
                strlcpy(wifi_status, "Unable to queue WiFi connection request", sizeof(wifi_status));
            }
        }
    } else if (action == static_cast<intptr_t>(WifiAction::Close)) {
        memset(password, 0, sizeof(password));
        current_screen = Screen::Dashboard;
        lv_screen_load(landing_screen);
    } else if (action >= static_cast<intptr_t>(WifiAction::FirstKey) &&
               action < static_cast<intptr_t>(WifiAction::FirstKey) + 50) {
        const size_t index = static_cast<size_t>(action - static_cast<intptr_t>(WifiAction::FirstKey));
        char *field = password_selected ? password : ssid;
        const size_t capacity = password_selected ? sizeof(password) : sizeof(ssid);
        const size_t length = strlen(field);
        if (length + 1 < capacity) {
            field[length] = keyboards[keyboard_page][index];
            field[length + 1] = '\0';
        }
    }

    update_wifi_fields();
    update_field_selection();
}

lv_obj_t *create_field(lv_obj_t *parent, int y, bool is_password)
{
    lv_obj_t *field = lv_obj_create(parent);
    lv_obj_set_pos(field, 24, y);
    lv_obj_set_size(field, 976, 40);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(field, is_password ? Card : Accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(field, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(field, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(field, 8, LV_PART_MAIN);
    lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(field, wifi_action_event, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(
                            is_password ? WifiAction::SelectPassword : WifiAction::SelectSsid)));

    lv_obj_t *label = lv_label_create(field);
    lv_obj_set_pos(label, 4, 2);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, is_password ? Text : Background, LV_PART_MAIN);
    return field;
}

void create_wifi_screen()
{
    wifi_screen = lv_obj_create(nullptr);
    lv_obj_set_size(wifi_screen, 1024, 600);
    lv_obj_remove_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(wifi_screen, Background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wifi_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wifi_screen, 0, LV_PART_MAIN);

    add_label(wifi_screen, 32, 28, "CONNECT TO WIFI", Text, &lv_font_montserrat_24);
    add_label(wifi_screen, 32, 76, "SSID", Muted, &lv_font_montserrat_20);
    ssid_field = create_field(wifi_screen, 98, false);
    ssid_label = lv_obj_get_child(ssid_field, 0);
    add_label(wifi_screen, 32, 146, "PASSWORD (tap field to select)", Muted, &lv_font_montserrat_20);
    password_field = create_field(wifi_screen, 168, true);
    password_label = lv_obj_get_child(password_field, 0);

    wifi_feedback = lv_label_create(wifi_screen);
    lv_obj_set_pos(wifi_feedback, 32, 225);
    lv_obj_set_width(wifi_feedback, 960);
    lv_label_set_long_mode(wifi_feedback, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(wifi_feedback, Muted, LV_PART_MAIN);

    for (size_t i = 0; i < 50; ++i) {
        const int x = 32 + static_cast<int>(i % 10) * 96;
        const int y = 274 + static_cast<int>(i / 10) * 44;
        lv_obj_t *button = create_button(wifi_screen, x, y, 88, 38, "", false);
        lv_obj_add_event_cb(button, wifi_action_event, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(
                                static_cast<int>(WifiAction::FirstKey) + i)));
        key_labels[i] = lv_obj_get_child(button, 0);
    }

    const WifiAction actions[] = {WifiAction::Delete, WifiAction::Space,
                                  WifiAction::ChangeKeyboard, WifiAction::Connect,
                                  WifiAction::Close};
    const char *labels[] = {"Delete", "Space", "abc/ABC/#!", "Connect", "Close"};
    const int xs[] = {32, 194, 356, 680, 842};
    const int widths[] = {150, 150, 150, 150, 150};
    for (size_t i = 0; i < 5; ++i) {
        lv_obj_t *button = create_button(wifi_screen, xs[i], 510, widths[i], 52,
                                         labels[i], actions[i] == WifiAction::Connect);
        lv_obj_add_event_cb(button, wifi_action_event, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(actions[i])));
    }
    add_label(wifi_screen, 32, 576,
              "WPA/WPA2 Personal or open WiFi / credentials saved after connection", Muted);
}
}

extern "C" void landing_screen_init()
{
    landing_screen = lv_obj_create(nullptr);
    lv_obj_set_size(landing_screen, 1024, 600);
    lv_obj_remove_flag(landing_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(landing_screen, Background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(landing_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(landing_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(landing_screen, 0, LV_PART_MAIN);

    add_label(landing_screen, 24, 18, "FIELD STATION", Text, &lv_font_montserrat_24);
    add_label(landing_screen, 26, 52, "ENERGY / LOCATION / LORAWAN", Muted);

    lv_obj_t *wifi_button = create_button(landing_screen, 802, 18, 198, 44, "WiFi setup", false);
    lv_obj_add_event_cb(wifi_button, wifi_button_event, LV_EVENT_CLICKED, nullptr);

    // Two compact rows: solar panel -> inverter, and below it battery -> inverter.
    const char *titles[INA_SENSOR_COUNT][3] = {
        {"SOLAR VOLTAGE", "SOLAR CURRENT", "SOLAR POWER"},
        {"BATT VOLTAGE", "BATT CURRENT", "BATT POWER"},
    };
    const char *values[] = {"-- V", "-- A", "-- W"};
    // Three columns of 256 px, and on the right a card as tall as both rows for the battery %.
    constexpr int ColumnWidth = 256;
    constexpr int ColumnStep = ColumnWidth + 16;
    for (size_t row = 0; row < INA_SENSOR_COUNT; ++row) {
        for (int i = 0; i < 3; ++i) {
            lv_obj_t *card = create_card(landing_screen, 24 + i * ColumnStep,
                                         84 + static_cast<int>(row) * 54, ColumnWidth, 48);
            lv_obj_t *title = lv_label_create(card);
            lv_label_set_text(title, titles[row][i]);
            lv_obj_set_style_text_color(title, Muted, LV_PART_MAIN);
            lv_obj_align(title, LV_ALIGN_LEFT_MID, 2, 0);

            lv_obj_t *value = lv_label_create(card);
            lv_obj_set_style_text_color(value, i == 2 ? Accent : Text, LV_PART_MAIN);
            lv_obj_set_style_text_font(value, &lv_font_montserrat_24, LV_PART_MAIN);
            lv_label_set_text(value, values[i]);
            lv_obj_align(value, LV_ALIGN_RIGHT_MID, -2, 0);
            energy_labels[row][i] = value;
        }
    }

    lv_obj_t *battery_card =
        create_card(landing_screen, 24 + 3 * ColumnStep, 84, 1000 - (24 + 3 * ColumnStep), 102);
    lv_obj_t *battery_title = lv_label_create(battery_card);
    lv_label_set_text(battery_title, "BATTERY");
    lv_obj_set_style_text_color(battery_title, Muted, LV_PART_MAIN);
    lv_obj_align(battery_title, LV_ALIGN_TOP_MID, 0, -6);
    battery_percent_label = lv_label_create(battery_card);
    lv_obj_set_style_text_color(battery_percent_label, Accent, LV_PART_MAIN);
    lv_obj_set_style_text_font(battery_percent_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(battery_percent_label, "--%");
    lv_obj_align(battery_percent_label, LV_ALIGN_BOTTOM_MID, 0, 6);

    dashboard_status = lv_label_create(landing_screen);
    lv_obj_set_pos(dashboard_status, 28, 194);
    lv_obj_set_width(dashboard_status, 960);
    lv_obj_set_style_text_color(dashboard_status, Muted, LV_PART_MAIN);

    // Bottom row, over the full width: temperature, GPS, and LoRaWAN with the buttons.
    constexpr int BottomY = 222;
    constexpr int BottomHeight = 576 - BottomY;

    lv_obj_t *temperature = create_card(landing_screen, 24, BottomY, 312, BottomHeight);
    add_label(temperature, 16, 16, "TEMPERATURE", Accent, &lv_font_montserrat_20);
    temperature_label = lv_label_create(temperature);
    lv_obj_set_style_text_color(temperature_label, Text, LV_PART_MAIN);
    lv_obj_set_style_text_font(temperature_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(temperature_label, "-- \xC2\xB0" "C");
    lv_obj_align(temperature_label, LV_ALIGN_CENTER, 0, -6);
    temperature_status_label = lv_label_create(temperature);
    lv_obj_set_style_text_color(temperature_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(temperature_status_label, "Waiting for DS18B20...");
    lv_obj_align(temperature_status_label, LV_ALIGN_CENTER, 0, 44);

    lv_obj_t *gps = create_card(landing_screen, 352, BottomY, 312, BottomHeight);
    add_label(gps, 16, 16, "GPS POSITION", Accent, &lv_font_montserrat_20);
    gps_status_label = lv_label_create(gps);
    lv_obj_set_pos(gps_status_label, 16, 52);
    lv_obj_set_width(gps_status_label, 248);
    lv_label_set_long_mode(gps_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(gps_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(gps_status_label, gps_status);

    lv_obj_t *status = create_card(landing_screen, 680, BottomY, 320, BottomHeight);
    add_label(status, 16, 16, "TTN / CLASS A", Accent, &lv_font_montserrat_20);
    lora_status_label = lv_label_create(status);
    lv_obj_set_pos(lora_status_label, 16, 52);
    lv_obj_set_size(lora_status_label, 256, 72);
    lv_label_set_long_mode(lora_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lora_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(lora_status_label, lora_status);

    downlink_status_label = lv_label_create(status);
    lv_obj_set_pos(downlink_status_label, 16, 132);
    lv_obj_set_size(downlink_status_label, 256, 72);
    lv_label_set_long_mode(downlink_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(downlink_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(downlink_status_label, downlink_status);

    const char *uplink_labels[] = {"Temp", "Energy", "GPS"};
    const LoraUplink uplinks[] = {LoraUplink::Temperature, LoraUplink::Energy,
                                  LoraUplink::Gps};
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *uplink_button = create_button(
            status, 16 + static_cast<int>(i) * 88, 214, 80, 40,
            uplink_labels[i], false);
        lv_obj_add_event_cb(uplink_button, lora_uplink_button_event,
                            LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(
                                static_cast<intptr_t>(uplinks[i])));
    }

    lv_obj_t *button = create_button(status, 16, 266, 256, 48, "Mijn schermen", true);
    lv_obj_add_event_cb(button, open_existing_screens, LV_EVENT_CLICKED, nullptr);

    create_wifi_screen();
    update_wifi_fields();
    lv_screen_load(landing_screen);
}

// The DS18B20 has no status queue: look at its latest reading once a second.
void update_temperature()
{
    static uint32_t last_ms;
    static bool first = true;
    const uint32_t now = millis();
    if (!temperature_label || (!first && now - last_ms < 1000)) {
        return;
    }
    first = false;
    last_ms = now;
    float celsius = 0.0f;
    if (temp_service_latest(&celsius)) {
        // LVGL's own printf has no %f: format with snprintf.
        char text[24];
        snprintf(text, sizeof(text), "%.1f \xC2\xB0" "C", celsius);
        lv_label_set_text(temperature_label, text);
        lv_label_set_text_fmt(temperature_status_label, "DS18B20 on GPIO%d", TEMP_GPIO_DATA);
    } else {
        lv_label_set_text(temperature_label, "-- \xC2\xB0" "C");
        lv_label_set_text(temperature_status_label, "Sensor not found");
    }
}

extern "C" void landing_screen_tick()
{
    update_temperature();
    if (wifi_read_status(wifi_status, sizeof(wifi_status))) {
        update_wifi_fields();
    }
    if (sd_service_read_status(sd_status, sizeof(sd_status))) {
        update_wifi_fields();
    }
    if (lora_service_read_status(lora_status, sizeof(lora_status)) &&
        lora_status_label) {
        lv_label_set_text(lora_status_label, lora_status);
    }
    if (lora_service_read_downlink(downlink_status, sizeof(downlink_status)) &&
        downlink_status_label) {
        lv_label_set_text(downlink_status_label, downlink_status);
    }
    if (gps_service_read_status(gps_status, sizeof(gps_status)) &&
        gps_status_label) {
        lv_label_set_text(gps_status_label, gps_status);
    }
    for (size_t sensor = 0; sensor < INA_SENSOR_COUNT; ++sensor) {
        InaReading energy{};
        if (ina_service_read(static_cast<InaSensor>(sensor), &energy) &&
            energy_labels[sensor][0]) {
            update_energy_labels(energy_labels[sensor], energy);
            if (sensor == INA_BATTERY && battery_percent_label) {
                if (energy.connected && energy.has_battery_percent) {
                    lv_label_set_text_fmt(battery_percent_label, "%u%%", energy.battery_percent);
                } else {
                    lv_label_set_text(battery_percent_label, "--%");
                }
            }
        }
    }
    // Nothing on the dashboard shows it any more; reading keeps the status queue empty.
    audio_service_read_status(audio_status, sizeof(audio_status));
}
