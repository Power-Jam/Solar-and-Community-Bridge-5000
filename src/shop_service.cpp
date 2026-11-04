#include "shop_service.h"

#include "eez-flow.h"
#include "screen_backgrounds.h"
#include "screens.h"
#include "users_service.h"
#include "vars.h"

#include <lvgl.h>

#include <cstdio>

namespace
{
constexpr size_t ItemCount = 12;
constexpr int32_t Price = 20;

// Items in the same group are worn in the same place, so wearing one takes off the others.
enum Group : uint8_t { Head, Neck, Back, Top, Scarf, Feet };

struct Item {
    lv_obj_t *const *card;
    lv_obj_t *const *badge;
    lv_obj_t *const *badge_label;
    Group group;
};

#define SHOP_ITEM(n, group) \
    {&objects.shop_item_##n##__card, &objects.shop_item_##n##__owned_badge, &objects.shop_item_##n##__owned_label, group}

// In the order of the shop grid, row by row: hat, cap, beanie, necklace, backpack,
// three shirts, scarf and three pairs of shoes.
const Item shop_items[ItemCount] = {
    SHOP_ITEM(1, Head), SHOP_ITEM(2, Head),  SHOP_ITEM(3, Head),  SHOP_ITEM(4, Neck),
    SHOP_ITEM(5, Back), SHOP_ITEM(6, Top),   SHOP_ITEM(7, Top),   SHOP_ITEM(8, Top),
    SHOP_ITEM(9, Scarf), SHOP_ITEM(10, Feet), SHOP_ITEM(11, Feet), SHOP_ITEM(12, Feet),
};

// The profile's Outfit preview has one slot per group, in Group order.
constexpr size_t OutfitSlots = Feet + 1;
lv_obj_t *const *outfit_images[OutfitSlots] = {
    &objects.outfit_item_1, &objects.outfit_item_2, &objects.outfit_item_3,
    &objects.outfit_item_4, &objects.outfit_item_5, &objects.outfit_item_6,
};

// The dress-up figure on Profile and Shop: layers drawn bottom to top, all on one 200x300 canvas.
enum FigureLayer : uint8_t { BagBack, HairBack, Base, Shoes, TopLayer, NeckLayer, ScarfLayer, BagFront, HairFront, Hat, LayerCount };
constexpr uint8_t SeatedAvatar = 5;  // uses a wheelchair, so its shoes sit higher
lv_obj_t *figure_layers[2][LayerCount];

size_t selected = 0;
uint32_t shown_files = UINT32_MAX;

uint16_t item_bit(size_t item)
{
    return static_cast<uint16_t>(1u << item);
}

int32_t coins()
{
    return eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_CURRENT_COINS).getInt32();
}

// Shows the copy in PSRAM; until it has loaded (or without a card) the image stays hidden.
void set_sd_image(lv_obj_t *image, const char *path)
{
    const void *loaded = screen_backgrounds_image(path);
    if (loaded) {
        lv_image_set_src(image, loaded);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

void refresh_cards()
{
    const uint16_t owned = users_owned_items();
    const uint16_t worn = users_worn_items();
    for (size_t i = 0; i < ItemCount; ++i) {
        lv_obj_t *badge = *shop_items[i].badge;
        if (owned & item_bit(i)) {
            lv_label_set_text(*shop_items[i].badge_label, worn & item_bit(i) ? "Wearing" : "Owned");
            lv_obj_remove_flag(badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void refresh_outfit()
{
    for (lv_obj_t *const *image : outfit_images) {
        set_sd_image(*image, "S:/assets/shop/outfit-empty.jpg");
    }
    const uint16_t worn = users_worn_items();
    for (size_t i = 0; i < ItemCount; ++i) {
        if (worn & item_bit(i)) {
            char path[44];
            snprintf(path, sizeof(path), "S:/assets/shop/item-%u-outfit.jpg", static_cast<unsigned>(i + 1));
            set_sd_image(*outfit_images[shop_items[i].group], path);
        }
    }
}

// The worn item of a group, or -1.
int worn_in(Group group)
{
    const uint16_t worn = users_worn_items();
    for (size_t i = 0; i < ItemCount; ++i) {
        if (shop_items[i].group == group && (worn & item_bit(i))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void refresh_figures()
{
    const unsigned avatar = users_current_avatar();
    const bool seated = avatar == SeatedAvatar;
    const char *const under_hat = worn_in(Head) >= 0 ? "-hat" : "";
    char paths[LayerCount][44] = {};
    if (avatar) {
        const auto item = [&paths](FigureLayer layer, int index, const char *suffix) {
            if (index >= 0) {
                snprintf(paths[layer], sizeof(paths[layer]), "S:/assets/figure/item-%d%s.lay", index + 1, suffix);
            }
        };
        const int bag = worn_in(Back);
        item(BagBack, bag, "-back");
        item(BagFront, bag, "");
        snprintf(paths[HairBack], sizeof(paths[HairBack]), "S:/assets/figure/hair-back-%u%s.lay", avatar, under_hat);
        snprintf(paths[Base], sizeof(paths[Base]), "S:/assets/figure/base-%u.lay", avatar);
        const int shoes = worn_in(Feet);
        if (shoes >= 0) {
            item(Shoes, shoes, seated ? "-seated" : "");
        } else {
            snprintf(paths[Shoes], sizeof(paths[Shoes]), "S:/assets/figure/shoes-default%s.lay", seated ? "-seated" : "");
        }
        item(TopLayer, worn_in(Top), "");
        item(NeckLayer, worn_in(Neck), "");
        item(ScarfLayer, worn_in(Scarf), "");
        snprintf(paths[HairFront], sizeof(paths[HairFront]), "S:/assets/figure/hair-front-%u%s.lay", avatar, under_hat);
        item(Hat, worn_in(Head), "");
    }
    for (lv_obj_t *(&layers)[LayerCount] : figure_layers) {
        for (size_t layer = 0; layer < LayerCount; ++layer) {
            int16_t x = 0;
            int16_t y = 0;
            const void *image = paths[layer][0] ? screen_backgrounds_layer(paths[layer], &x, &y) : nullptr;
            if (image) {
                lv_image_set_src(layers[layer], image);
                lv_obj_set_pos(layers[layer], x, y);
                lv_obj_remove_flag(layers[layer], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(layers[layer], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void refresh_popup()
{
    char path[40];
    snprintf(path, sizeof(path), "S:/assets/shop/item-%u.jpg", static_cast<unsigned>(selected + 1));
    set_sd_image(objects.overlay_item, path);

    char detail[48];
    lv_obj_t *action = objects.overlay_action;
    lv_obj_remove_state(action, LV_STATE_DISABLED);
    if (!(users_owned_items() & item_bit(selected))) {
        set_sd_image(objects.overlay_action_icon, "S:/assets/shop/icon-buy.jpg");
        lv_label_set_text(objects.overlay_action_title, "Buy");
        if (coins() >= Price) {
            snprintf(detail, sizeof(detail), "%d coins", static_cast<int>(Price));
        } else {
            snprintf(detail, sizeof(detail), "You need %d more coins", static_cast<int>(Price - coins()));
            lv_obj_add_state(action, LV_STATE_DISABLED);
        }
    } else {
        set_sd_image(objects.overlay_action_icon, "S:/assets/shop/icon-wear.jpg");
        if (users_worn_items() & item_bit(selected)) {
            lv_label_set_text(objects.overlay_action_title, "Take off");
            snprintf(detail, sizeof(detail), "You are wearing this item");
        } else {
            lv_label_set_text(objects.overlay_action_title, "Wear");
            snprintf(detail, sizeof(detail), "Put it on your avatar");
        }
    }
    lv_label_set_text(objects.overlay_action_detail, detail);
}

void close_popup()
{
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SHOW_OVERLAY, eez::BooleanValue(false));
}

// The card's flow opens the popup; this picks what it shows.
void on_card_clicked(lv_event_t *event)
{
    selected = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ITEM_COST, eez::Value(static_cast<int>(Price)));
    refresh_popup();
}

void on_action(lv_event_t *)
{
    uint16_t owned = users_owned_items();
    uint16_t worn = users_worn_items();
    if (!(owned & item_bit(selected))) {
        if (coins() < Price) {
            return;
        }
        // users_service saves the new balance on its next tick.
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CURRENT_COINS, eez::Value(static_cast<int>(coins() - Price)));
        owned |= item_bit(selected);
    } else if (worn & item_bit(selected)) {
        worn &= ~item_bit(selected);
    } else {
        for (size_t i = 0; i < ItemCount; ++i) {
            if (shop_items[i].group == shop_items[selected].group) {
                worn &= ~item_bit(i);
            }
        }
        worn |= item_bit(selected);
    }
    users_set_items(owned, worn);
    refresh_cards();
    refresh_figures();
    close_popup();
}

void on_shop_load(lv_event_t *)
{
    refresh_cards();
    refresh_figures();
}

void on_profile_load(lv_event_t *)
{
    refresh_outfit();
    refresh_figures();
}
}

void shop_init()
{
    for (size_t i = 0; i < ItemCount; ++i) {
        lv_obj_add_event_cb(*shop_items[i].card, on_card_clicked, LV_EVENT_CLICKED, reinterpret_cast<void *>(i));
        // The badge covers the card's price; taps on it still open the card.
        lv_obj_remove_flag(*shop_items[i].badge, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(objects.overlay_action, on_action, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_style_opa(objects.overlay_action, LV_OPA_50, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_add_event_cb(objects.shop_page, on_shop_load, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_obj_add_event_cb(objects.profile_page, on_profile_load, LV_EVENT_SCREEN_LOAD_START, nullptr);
    lv_obj_t *const figures[] = {objects.profile_figure, objects.shop_figure};
    for (size_t f = 0; f < 2; ++f) {
        lv_obj_remove_flag(figures[f], LV_OBJ_FLAG_SCROLLABLE);
        for (lv_obj_t *&layer : figure_layers[f]) {
            layer = lv_image_create(figures[f]);
            lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
        }
    }
    refresh_cards();
    refresh_outfit();
    refresh_figures();
}

void shop_tick()
{
    // Pictures arrive from the SD card after start-up; show them as they do.
    if (screen_backgrounds_version() != shown_files) {
        shown_files = screen_backgrounds_version();
        refresh_outfit();
        refresh_figures();
    }
}
