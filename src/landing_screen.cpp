#include "ui.h"

#include "eez-flow.h"
#include "screens.h"
#include "sd_service.h"
#include "lora_service.h"
#include "gps_service.h"
#include "ina_service.h"
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
lv_obj_t *sd_image;
lv_obj_t *sd_image_placeholder;
lv_obj_t *sd_status_label;
lv_obj_t *audio_status_label;
lv_obj_t *lora_status_label;
lv_obj_t *gps_status_label;
lv_obj_t *downlink_status_label;
lv_obj_t *energy_labels[3];
lv_image_dsc_t sd_image_descriptor{};
uint16_t *sd_image_pixels;
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

void update_energy_labels(const InaReading &reading)
{
    char text[32];
    if (!reading.connected) {
        lv_label_set_text(energy_labels[0], "-- V");
        lv_label_set_text(energy_labels[1], "-- A");
        lv_label_set_text(energy_labels[2], "-- W");
        return;
    }

    snprintf(text, sizeof(text), "%.2f V", reading.voltage);
    lv_label_set_text(energy_labels[0], text);
    if (reading.over_range) {
        lv_label_set_text(energy_labels[1], "OVER RANGE");
        lv_label_set_text(energy_labels[2], "OVER RANGE");
        return;
    }
    snprintf(text, sizeof(text), "%.3f A", reading.current);
    lv_label_set_text(energy_labels[1], text);
    snprintf(text, sizeof(text), "%.2f W", reading.power);
    lv_label_set_text(energy_labels[2], text);
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

void sd_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        const SdCommand command = static_cast<SdCommand>(
            reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
        if (!sd_service_request(command)) {
            strlcpy(sd_status, "SD request queue full", sizeof(sd_status));
            if (sd_status_label) {
                lv_label_set_text(sd_status_label, sd_status);
            }
        }
    }
}

void lora_test_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        !lora_service_request_test_uplink()) {
        strlcpy(lora_status, "LoRaWAN request queue unavailable",
                sizeof(lora_status));
        if (lora_status_label) {
            lv_label_set_text(lora_status_label, lora_status);
        }
    }
}

void play_audio_button_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        !sd_service_request(SdCommand::PlayAudio)) {
        strlcpy(audio_status, "Unable to queue audio request",
                sizeof(audio_status));
        if (audio_status_label) {
            lv_label_set_text_fmt(audio_status_label, "Audio: %s", audio_status);
        }
    }
}

void update_sd_image(const SdImage &image)
{
    if (!sd_image) {
        free(image.pixels);
        return;
    }

    lv_image_set_src(sd_image, nullptr);
    free(sd_image_pixels);
    sd_image_pixels = nullptr;
    if (!image.pixels) {
        sd_image_descriptor.data = nullptr;
        sd_image_descriptor.data_size = 0;
        sd_image_descriptor.header.w = 0;
        sd_image_descriptor.header.h = 0;
        lv_obj_remove_flag(sd_image_placeholder, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    sd_image_pixels = image.pixels;

    sd_image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    sd_image_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    sd_image_descriptor.header.flags = 0;
    sd_image_descriptor.header.w = image.width;
    sd_image_descriptor.header.h = image.height;
    sd_image_descriptor.header.stride = image.width * sizeof(uint16_t);
    sd_image_descriptor.data_size = static_cast<uint32_t>(
        image.width * image.height * sizeof(uint16_t));
    sd_image_descriptor.data = reinterpret_cast<const uint8_t *>(sd_image_pixels);
    lv_image_set_src(sd_image, &sd_image_descriptor);
    lv_obj_center(sd_image);
    lv_obj_add_flag(sd_image_placeholder, LV_OBJ_FLAG_HIDDEN);
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
        eez_flow_set_screen(SCREEN_ID_HOME_PAGE, LV_SCR_LOAD_ANIM_NONE, 0, 0);
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

    const char *titles[] = {"VOLTAGE", "CURRENT", "POWER"};
    const char *values[] = {"-- V", "-- A", "-- W"};
    for (int i = 0; i < 3; ++i) {
        const int x = 24 + i * 328;
        create_card(landing_screen, x, 84, 312, 106);
        add_label(landing_screen, x + 18, 98, titles[i], Muted, &lv_font_montserrat_20);
        energy_labels[i] = lv_label_create(landing_screen);
        lv_obj_set_pos(energy_labels[i], x + 18, 132);
        lv_obj_set_style_text_color(energy_labels[i], i == 2 ? Accent : Text, LV_PART_MAIN);
        lv_obj_set_style_text_font(energy_labels[i], &lv_font_montserrat_24, LV_PART_MAIN);
        lv_label_set_text(energy_labels[i], values[i]);
    }

    dashboard_status = lv_label_create(landing_screen);
    lv_obj_set_pos(dashboard_status, 28, 194);
    lv_obj_set_width(dashboard_status, 960);
    lv_obj_set_style_text_color(dashboard_status, Muted, LV_PART_MAIN);

    lv_obj_t *overview = create_card(landing_screen, 24, 210, 640, 366);
    add_label(overview, 24, 12, "SD CARD / PHOTO VIEWER", Accent,
              &lv_font_montserrat_20);
    lv_obj_t *image_view = lv_obj_create(overview);
    lv_obj_set_pos(image_view, 16, 46);
    lv_obj_set_size(image_view, 608, 208);
    lv_obj_remove_flag(image_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(image_view, Background, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(image_view, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(image_view, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(image_view, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(image_view, 0, LV_PART_MAIN);

    sd_image = lv_image_create(image_view);
    lv_obj_center(sd_image);
    sd_image_placeholder = lv_label_create(image_view);
    lv_label_set_text(sd_image_placeholder, "Add JPG/JPEG files to /images on the SD card");
    lv_obj_set_style_text_color(sd_image_placeholder, Muted, LV_PART_MAIN);
    lv_obj_center(sd_image_placeholder);

    sd_status_label = lv_label_create(overview);
    lv_obj_set_pos(sd_status_label, 16, 258);
    lv_obj_set_width(sd_status_label, 608);
    lv_label_set_long_mode(sd_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(sd_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(sd_status_label, sd_status);

    audio_status_label = lv_label_create(overview);
    lv_obj_set_pos(audio_status_label, 16, 278);
    lv_obj_set_width(audio_status_label, 608);
    lv_label_set_long_mode(audio_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(audio_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text_fmt(audio_status_label, "Audio: %s", audio_status);

    const char *sd_labels[] = {"Previous", "Next", "Retry SD", "Play WAV"};
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_t *button = create_button(overview, 16 + static_cast<int>(i) * 150,
                                         302, 134, 40, sd_labels[i],
                                         i == 1);
        if (i == 3) {
            lv_obj_add_event_cb(button, play_audio_button_event,
                                LV_EVENT_CLICKED, nullptr);
            continue;
        }
        const SdCommand sd_commands[] = {
            SdCommand::Previous, SdCommand::Next, SdCommand::Retry
        };
        lv_obj_add_event_cb(button, sd_button_event, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(
                                static_cast<intptr_t>(sd_commands[i])));
    }

    lv_obj_t *status = create_card(landing_screen, 688, 210, 312, 366);
    add_label(status, 16, 16, "GPS POSITION", Accent, &lv_font_montserrat_20);
    gps_status_label = lv_label_create(status);
    lv_obj_set_pos(gps_status_label, 16, 52);
    lv_obj_set_width(gps_status_label, 280);
    lv_label_set_long_mode(gps_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(gps_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(gps_status_label, gps_status);
    add_label(status, 16, 112, "TTN / CLASS A", Accent, &lv_font_montserrat_20);
    lora_status_label = lv_label_create(status);
    lv_obj_set_pos(lora_status_label, 16, 148);
    lv_obj_set_width(lora_status_label, 280);
    lv_label_set_long_mode(lora_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lora_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(lora_status_label, lora_status);

    downlink_status_label = lv_label_create(status);
    lv_obj_set_pos(downlink_status_label, 16, 188);
    lv_obj_set_width(downlink_status_label, 280);
    lv_label_set_long_mode(downlink_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(downlink_status_label, Muted, LV_PART_MAIN);
    lv_label_set_text(downlink_status_label, downlink_status);

    lv_obj_t *test_uplink_button = create_button(
        status, 16, 238, 264, 40, "Send test uplink", false);
    lv_obj_add_event_cb(test_uplink_button, lora_test_button_event,
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *button = create_button(status, 16, 290, 264, 48, "Mijn schermen", true);
    lv_obj_add_event_cb(button, open_existing_screens, LV_EVENT_CLICKED, nullptr);

    create_wifi_screen();
    update_wifi_fields();
    lv_screen_load(landing_screen);
}

extern "C" void landing_screen_tick()
{
    if (wifi_read_status(wifi_status, sizeof(wifi_status))) {
        update_wifi_fields();
    }
    if (sd_service_read_status(sd_status, sizeof(sd_status))) {
        if (sd_status_label) {
            lv_label_set_text(sd_status_label, sd_status);
        }
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
    InaReading energy{};
    if (ina_service_read(&energy) && energy_labels[0]) {
        update_energy_labels(energy);
    }
    if (audio_service_read_status(audio_status, sizeof(audio_status)) &&
        audio_status_label) {
        lv_label_set_text_fmt(audio_status_label, "Audio: %s", audio_status);
    }
    SdImage image{};
    if (sd_service_take_image(&image)) {
        update_sd_image(image);
    }
}
