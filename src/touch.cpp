#include <Arduino.h>
#include <Wire.h>

#include "board_config.h"
#include "eez-flow.h"
#include "screens.h"
#include "touch.h"

namespace {
constexpr uint8_t TouchFailureLimit = 5;
constexpr uint32_t TouchRetryIntervalMs = 1000;

uint8_t touch_address = 0x5D;
bool touch_available = false;
bool touch_pressed = false;
uint8_t touch_failures = 0;
uint32_t touch_retry_at = 0;

/**
 * @brief Keep the last state through short I2C errors and slow down polling after repeated ones.
 */
bool record_touch_failure(bool & pressed)
{
    if (touch_failures < TouchFailureLimit) {
        ++touch_failures;
        if (touch_failures == TouchFailureLimit) {
            Serial.println("GT911 not responding; retrying every second");
        }
    }
    if (touch_failures == TouchFailureLimit) {
        touch_pressed = false;
        pressed = false;
        touch_retry_at = millis() + TouchRetryIntervalMs;
    }
    return false;
}

void record_touch_success()
{
    if (touch_failures == TouchFailureLimit) {
        Serial.println("GT911 responding again");
    }
    touch_failures = 0;
}

bool read_touch_register(uint16_t reg, uint8_t *data, size_t length)
{
    Wire.beginTransmission(touch_address);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg));
    bool ok = Wire.endTransmission(false) == 0;
    if (ok) {
        ok = Wire.requestFrom(touch_address, length) == length;
    }
    if (ok) {
        for (size_t i = 0; i < length; ++i) {
            data[i] = Wire.read();
        }
    }
    return ok;
}

bool write_touch_register(uint16_t reg, uint8_t value)
{
    Wire.beginTransmission(touch_address);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg));
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

void navigate_on_click(lv_event_t * event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const int16_t screen_id = static_cast<int16_t>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    eez_flow_set_screen(screen_id, LV_SCR_LOAD_ANIM_NONE, 0, 0);
}

void touch_read(lv_indev_t *, lv_indev_data_t * data)
{
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    static bool was_pressed = false;
    const bool previous_pressed = was_pressed;
    bool pressed = was_pressed;
    touch_read_state(last_x, last_y, pressed);
    was_pressed = pressed;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = last_x;
    data->point.y = last_y;
    if (pressed && !previous_pressed) {
        Serial.printf("Touch %d,%d\n", last_x, last_y);
    }
}

void register_navigation(lv_obj_t * button, int16_t screen_id, const char * name)
{
    if (button) {
        lv_obj_add_event_cb(button, navigate_on_click, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(screen_id)));
    } else {
        Serial.printf("%s button was not created\n", name);
    }
}

}

/**
 * @brief Confirm the touch controller was initialized with the display board.
 */
bool touch_init(bool i2c_available)
{
    if (!i2c_available) {
        Serial.println("GT911 touch controller unavailable: I2C bus failed");
        return false;
    }

    pinMode(TOUCH_GPIO_INT, OUTPUT);
    digitalWrite(TOUCH_GPIO_INT, LOW);
    pinMode(TOUCH_GPIO_RST, OUTPUT);
    digitalWrite(TOUCH_GPIO_RST, LOW);
    delay(15);
    digitalWrite(TOUCH_GPIO_RST, HIGH);
    delay(60);
    pinMode(TOUCH_GPIO_INT, INPUT);
    delay(60);

    uint8_t id[4] = {};
    if (!read_touch_register(0x8140, id, sizeof(id))) {
        touch_address = 0x14;
        if (!read_touch_register(0x8140, id, sizeof(id))) {
            touch_address = 0x5D;
            Serial.println("CrowPanel GT911 touch controller is unavailable");
            return false;
        }
    }
    touch_available = true;
    Serial.println("CrowPanel GT911 touch controller ready");
    return true;
}

/**
 * @brief Read the current GT911 point and clamp it to the panel.
 * @param x Output horizontal coordinate.
 * @param y Output vertical coordinate.
 * @param pressed Output contact state; kept unchanged during a short I2C error.
 * @return true when a fresh sample was read; false when no fresh sample is available.
 */
bool touch_read_state(int16_t & x, int16_t & y, bool & pressed)
{
    if (!touch_available) {
        pressed = false;
        return false;
    }
    if (touch_failures == TouchFailureLimit &&
        static_cast<int32_t>(millis() - touch_retry_at) < 0) {
        pressed = false;
        return false;
    }

    uint8_t status = 0;
    if (!read_touch_register(0x814E, &status, 1)) {
        return record_touch_failure(pressed);
    }

    if (status & 0x80) {
        const uint8_t points = status & 0x0F;
        const bool down = points > 0 && points <= 5;
        if (down) {
            uint8_t point[5] = {};
            if (!read_touch_register(0x814F, point, sizeof(point))) {
                return record_touch_failure(pressed);
            }
            x = constrain(static_cast<int16_t>(point[1] | (point[2] << 8)),
                          0, PANEL_WIDTH - 1);
            y = constrain(static_cast<int16_t>(point[3] | (point[4] << 8)),
                          0, PANEL_HEIGHT - 1);
        }
        touch_pressed = down;
        if (!write_touch_register(0x814E, 0)) {
            return record_touch_failure(pressed);
        }
    }
    record_touch_success();
    pressed = touch_pressed;
    return true;
}

/**
 * @brief Register the touch input and home-screen button navigation with LVGL.
 */
void touch_setup_lvgl(lv_display_t * display)
{
    lv_indev_t * touch_indev = lv_indev_create();
    if (!touch_indev) {
        Serial.println("Unable to create LVGL touch input");
        return;
    }

    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch_indev, display);
    lv_indev_set_read_cb(touch_indev, touch_read);

    register_navigation(objects.btn_users, SCREEN_ID_USER_LIST_VIEW, "Users");
    register_navigation(objects.btn_start, SCREEN_ID_ROADMAP, "Start");
}
