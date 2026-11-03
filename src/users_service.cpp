#include "users_service.h"

#include "eez-flow.h"
#include "screen_backgrounds.h"
#include "screens.h"
#include "ui.h"
#include "vars.h"

#include <Arduino.h>
#include <Preferences.h>
#include <lvgl.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace
{
constexpr size_t MaxUsers = 12;
constexpr size_t NameSize = 21;
constexpr size_t AvatarCount = 9;
constexpr uint8_t StoreVersion = 1;
constexpr int32_t FirstLevel = 1;

const lv_color_t Cream = lv_color_hex(0xFAF7EF);
const lv_color_t Card = lv_color_hex(0xF0EBDD);
const lv_color_t Green = lv_color_hex(0x2F6B45);
const lv_color_t Text = lv_color_hex(0x18352A);

struct User {
    char name[NameSize];
    uint8_t avatar;  // 1..AvatarCount, as numbered in the user creation grid
    int32_t coins;
    int32_t unlocked_level;
};

struct Store {
    uint8_t version;
    uint8_t count;
    uint8_t current;
    User users[MaxUsers];
};

Store store{};
Preferences preferences;
bool storage_ready = false;
bool redirect_to_creation = false;
uint32_t shown_version = UINT32_MAX;
// Which learner each list row shows, after the search filter.
int8_t row_user[MaxUsers];

lv_obj_t *creation_keyboard;
lv_obj_t *list_keyboard;

lv_obj_t *const *row_objects[MaxUsers] = {
    &objects.user_row_1, &objects.user_row_2, &objects.user_row_3, &objects.user_row_4,
    &objects.user_row_5, &objects.user_row_6, &objects.user_row_7, &objects.user_row_8,
    &objects.user_row_9, &objects.user_row_10, &objects.user_row_11, &objects.user_row_12,
};
lv_obj_t *const *row_avatars[MaxUsers] = {
    &objects.user_row_1_avatar, &objects.user_row_2_avatar, &objects.user_row_3_avatar,
    &objects.user_row_4_avatar, &objects.user_row_5_avatar, &objects.user_row_6_avatar,
    &objects.user_row_7_avatar, &objects.user_row_8_avatar, &objects.user_row_9_avatar,
    &objects.user_row_10_avatar, &objects.user_row_11_avatar, &objects.user_row_12_avatar,
};
lv_obj_t *const *row_names[MaxUsers] = {
    &objects.user_row_1_name, &objects.user_row_2_name, &objects.user_row_3_name,
    &objects.user_row_4_name, &objects.user_row_5_name, &objects.user_row_6_name,
    &objects.user_row_7_name, &objects.user_row_8_name, &objects.user_row_9_name,
    &objects.user_row_10_name, &objects.user_row_11_name, &objects.user_row_12_name,
};
lv_obj_t *const *avatar_choices[AvatarCount] = {
    &objects.action_user_creation_avatar_1_1, &objects.action_user_creation_avatar_2_2,
    &objects.action_user_creation_avatar_3_3, &objects.action_user_creation_avatar_4_4,
    &objects.action_user_creation_avatar_5_5, &objects.action_user_creation_avatar_6_6,
    &objects.action_user_creation_avatar_7_7, &objects.action_user_creation_avatar_8_8,
    &objects.action_user_creation_avatar_9_9,
};
// Both nav bar layouts on every screen with a nav bar.
lv_obj_t *const *nav_avatars[] = {
    &objects.nav_bar_chapters__avatar_image, &objects.nav_bar_chapters__avatar_image_no_coins,
    &objects.nav_bar_roadmap__avatar_image, &objects.nav_bar_roadmap__avatar_image_no_coins,
    &objects.nav_bar_level__avatar_image, &objects.nav_bar_level__avatar_image_no_coins,
    &objects.nav_bar_profile__avatar_image, &objects.nav_bar_profile__avatar_image_no_coins,
    &objects.nav_bar_shop__avatar_image, &objects.nav_bar_shop__avatar_image_no_coins,
};

User *current_user()
{
    return store.count ? &store.users[store.current] : nullptr;
}

void save()
{
    if (storage_ready && preferences.putBytes("store", &store, sizeof(store)) != sizeof(store)) {
        Serial.println("[Users] Saving learners failed");
    }
}

void load()
{
    storage_ready = preferences.begin("users", false);
    if (!storage_ready) {
        Serial.println("[Users] Storage unavailable; learners will not survive a restart");
        return;
    }
    Store loaded{};
    if (preferences.getBytes("store", &loaded, sizeof(loaded)) == sizeof(loaded) &&
        loaded.version == StoreVersion && loaded.count <= MaxUsers &&
        (loaded.count == 0 || loaded.current < loaded.count)) {
        store = loaded;
        for (size_t i = 0; i < store.count; ++i) {
            User &user = store.users[i];
            user.name[NameSize - 1] = '\0';
            if (user.avatar < 1 || user.avatar > AvatarCount) {
                user.avatar = 1;
            }
        }
    }
    store.version = StoreVersion;
    Serial.printf("[Users] %u learner(s) loaded\n", static_cast<unsigned>(store.count));
}

int32_t flow_int(uint32_t index)
{
    return eez::flow::getGlobalVariable(index).getInt32();
}

void set_flow_int(uint32_t index, int32_t value)
{
    eez::flow::setGlobalVariable(index, eez::Value(static_cast<int>(value)));
}

// The flows only know one learner: copy the selected learner into their variables.
void push_to_flow()
{
    const User *user = current_user();
    set_flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_COINS, user ? user->coins : 0);
    set_flow_int(FLOW_GLOBAL_VARIABLE_HIGHEST_UNLOCKED_LEVEL, user ? user->unlocked_level : FirstLevel);
}

void show_image(lv_obj_t *image, uint8_t avatar, const char *size)
{
    char path[48];
    snprintf(path, sizeof(path), "S:/assets/avatars/avatar-%u-%s.jpg", static_cast<unsigned>(avatar), size);
    const void *source = screen_backgrounds_image(path);
    if (source) {
        lv_image_set_src(image, source);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_hidden(lv_obj_t *object, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

// The carousel sides show first names, like the Figma.
void set_first_name(lv_obj_t *label, const char *name)
{
    char first[NameSize];
    strlcpy(first, name, sizeof(first));
    if (char *space = strchr(first, ' ')) {
        *space = '\0';
    }
    lv_label_set_text(label, first);
}

bool matches_search(const char *name, const char *search)
{
    if (!search[0]) {
        return true;
    }
    for (const char *start = name; *start; ++start) {
        size_t i = 0;
        while (search[i] && start[i] &&
               tolower(static_cast<unsigned char>(start[i])) == tolower(static_cast<unsigned char>(search[i]))) {
            ++i;
        }
        if (!search[i]) {
            return true;
        }
    }
    return false;
}

void refresh_list()
{
    const char *search = lv_textarea_get_text(objects.user_search);
    size_t row = 0;
    for (size_t i = 0; i < store.count; ++i) {
        if (matches_search(store.users[i].name, search)) {
            row_user[row] = static_cast<int8_t>(i);
            lv_label_set_text(*row_names[row], store.users[i].name);
            show_image(*row_avatars[row], store.users[i].avatar, "42");
            set_hidden(*row_objects[row], false);
            ++row;
        }
    }
    for (; row < MaxUsers; ++row) {
        row_user[row] = -1;
        set_hidden(*row_objects[row], true);
    }
}

void refresh()
{
    const User *user = current_user();
    const size_t count = store.count;
    if (user) {
        const User &previous = store.users[(store.current + count - 1) % count];
        const User &next = store.users[(store.current + 1) % count];
        lv_label_set_text(objects.home_name_center, user->name);
        show_image(objects.home_avatar_center, user->avatar, "210");
        set_first_name(objects.home_name_prev, previous.name);
        show_image(objects.home_avatar_prev, previous.avatar, "90");
        set_first_name(objects.home_name_next, next.name);
        show_image(objects.home_avatar_next, next.avatar, "90");
        lv_label_set_text(objects.profile_name, user->name);
        show_image(objects.profile_avatar, user->avatar, "78");
        show_image(objects.shop_avatar, user->avatar, "86");
        for (lv_obj_t *const *avatar : nav_avatars) {
            show_image(*avatar, user->avatar, "nav");
        }
    } else {
        lv_label_set_text(objects.home_name_center, "");
        lv_label_set_text(objects.profile_name, "");
        set_hidden(objects.home_avatar_center, true);
        set_hidden(objects.profile_avatar, true);
        set_hidden(objects.shop_avatar, true);
        for (lv_obj_t *const *avatar : nav_avatars) {
            set_hidden(*avatar, true);
        }
    }
    // With two learners the other one is both previous and next, so show it once, on the right.
    set_hidden(objects.home_prev, count < 3);
    set_hidden(objects.home_next, count < 2);
    refresh_list();
}

void select_user(size_t index)
{
    if (index >= store.count || index == store.current) {
        return;
    }
    store.current = static_cast<uint8_t>(index);
    push_to_flow();
    save();
    refresh();
}

void on_previous(lv_event_t *)
{
    if (store.count) {
        select_user((store.current + store.count - 1) % store.count);
    }
}

void on_next(lv_event_t *)
{
    if (store.count) {
        select_user((store.current + 1) % store.count);
    }
}

// The rows are not clickable, so taps land on the container, whose flow opens Home.
void on_rows_clicked(lv_event_t *)
{
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    for (size_t row = 0; row < MaxUsers; ++row) {
        lv_obj_t *object = *row_objects[row];
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        if (row_user[row] >= 0 && !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) &&
            lv_area_is_point_on(&area, &point, 0)) {
            select_user(static_cast<size_t>(row_user[row]));
            return;
        }
    }
}

void on_search_changed(lv_event_t *)
{
    refresh_list();
}

bool avatar_chosen()
{
    for (lv_obj_t *const *choice : avatar_choices) {
        if (lv_obj_has_state(*choice, LV_STATE_CHECKED)) {
            return true;
        }
    }
    return false;
}

bool name_typed()
{
    for (const char *c = lv_textarea_get_text(objects.user_name_input); *c; ++c) {
        if (*c != ' ') {
            return true;
        }
    }
    return false;
}

// A disabled button gets no clicks, so Confirm's flow cannot open Chapters until a name is typed and an avatar is chosen.
void update_confirm()
{
    if (name_typed() && avatar_chosen()) {
        lv_obj_remove_state(objects.action_user_creation_confirm_11, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(objects.action_user_creation_confirm_11, LV_STATE_DISABLED);
    }
}

void on_name_changed(lv_event_t *)
{
    update_confirm();
}

void on_avatar_choice(lv_event_t *event)
{
    lv_obj_t *chosen = static_cast<lv_obj_t *>(lv_event_get_target(event));
    for (lv_obj_t *const *choice : avatar_choices) {
        if (*choice == chosen) {
            lv_obj_add_state(*choice, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(*choice, LV_STATE_CHECKED);
        }
    }
    update_confirm();
}

// Confirm's flow opens Chapters; this runs first and makes the new learner the selected one.
void on_confirm(lv_event_t *)
{
    if (store.count >= MaxUsers) {
        Serial.println("[Users] Learner list is full");
        return;
    }
    if (!name_typed() || !avatar_chosen()) {
        return;
    }
    User user{};
    const char *typed = lv_textarea_get_text(objects.user_name_input);
    while (*typed == ' ') {
        ++typed;
    }
    strlcpy(user.name, typed, sizeof(user.name));
    for (size_t end = strlen(user.name); end > 0 && user.name[end - 1] == ' '; --end) {
        user.name[end - 1] = '\0';
    }
    for (size_t i = 0; i < AvatarCount; ++i) {
        if (lv_obj_has_state(*avatar_choices[i], LV_STATE_CHECKED)) {
            user.avatar = static_cast<uint8_t>(i + 1);
        }
    }
    user.coins = 0;
    user.unlocked_level = FirstLevel;
    store.users[store.count] = user;
    store.current = store.count;
    ++store.count;
    push_to_flow();
    save();
    refresh();
}

void on_creation_load(lv_event_t *)
{
    lv_textarea_set_text(objects.user_name_input, "");
    for (lv_obj_t *const *choice : avatar_choices) {
        lv_obj_remove_state(*choice, LV_STATE_CHECKED);
    }
    update_confirm();
    lv_obj_add_flag(creation_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void on_list_load(lv_event_t *)
{
    lv_textarea_set_text(objects.user_search, "");
    lv_obj_add_flag(list_keyboard, LV_OBJ_FLAG_HIDDEN);
    refresh_list();
}

// Without learners there is nothing to show on Home, so go straight to adding one.
void on_home_load(lv_event_t *)
{
    if (!store.count) {
        redirect_to_creation = true;
    }
}

void on_field_clicked(lv_event_t *event)
{
    lv_obj_t *keyboard = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    lv_keyboard_set_textarea(keyboard, static_cast<lv_obj_t *>(lv_event_get_target(event)));
    lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void on_keyboard_done(lv_event_t *event)
{
    lv_obj_t *keyboard = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_t *field = lv_keyboard_get_textarea(keyboard)) {
        lv_obj_remove_state(field, LV_STATE_FOCUSED);
    }
}

lv_obj_t *create_keyboard(lv_obj_t *screen, lv_obj_t *field)
{
    lv_obj_t *keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, 1024, 260);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(keyboard, Cream, LV_PART_MAIN);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(keyboard, Card, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, Text, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, Green, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(keyboard, Cream, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_keyboard_set_textarea(keyboard, field);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, on_keyboard_done, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, on_keyboard_done, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(field, on_field_clicked, LV_EVENT_CLICKED, keyboard);
    return keyboard;
}

// Learners are kept up to date with what the flows changed (coins earned, levels unlocked).
void pull_from_flow()
{
    User *user = current_user();
    if (!user) {
        return;
    }
    const int32_t coins = flow_int(FLOW_GLOBAL_VARIABLE_CURRENT_COINS);
    const int32_t level = flow_int(FLOW_GLOBAL_VARIABLE_HIGHEST_UNLOCKED_LEVEL);
    if (coins != user->coins || level != user->unlocked_level) {
        user->coins = coins;
        user->unlocked_level = level;
        save();
    }
}
}

void users_init()
{
    load();

    lv_textarea_set_max_length(objects.user_name_input, NameSize - 1);
    lv_textarea_set_max_length(objects.user_search, NameSize - 1);
    creation_keyboard = create_keyboard(objects.user_creation, objects.user_name_input);
    list_keyboard = create_keyboard(objects.user_list_view, objects.user_search);

    lv_obj_add_event_cb(objects.home_prev, on_previous, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.home_next, on_next, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.user_rows, on_rows_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(objects.user_search, on_search_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(objects.user_name_input, on_name_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    for (lv_obj_t *const *choice : avatar_choices) {
        lv_obj_add_event_cb(*choice, on_avatar_choice, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_add_event_cb(objects.action_user_creation_confirm_11, on_confirm, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_opa(objects.action_user_creation_confirm_11, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    update_confirm();
    lv_obj_add_event_cb(objects.user_creation, on_creation_load, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_obj_add_event_cb(objects.user_list_view, on_list_load, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_obj_add_event_cb(objects.home_page, on_home_load, LV_EVENT_SCREEN_LOADED, nullptr);

    push_to_flow();
    refresh();
}

void users_tick()
{
    pull_from_flow();
    if (redirect_to_creation) {
        redirect_to_creation = false;
        eez_flow_set_screen(SCREEN_ID_USER_CREATION, LV_SCR_LOAD_ANIM_NONE, 0, 0);
    }
    // Avatars arrive from the SD card after start-up; show them as they do.
    if (screen_backgrounds_version() != shown_version) {
        shown_version = screen_backgrounds_version();
        refresh();
    }
}
