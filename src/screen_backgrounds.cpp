#include "screen_backgrounds.h"

#include "eez-flow.h"
#include "screens.h"
#include "sd_service.h"

#include <Arduino.h>
#include <cstring>
#include <lvgl.h>

namespace
{
struct Background {
    lv_obj_t *const *widget;
    const char *path;
};

#define LEVEL_POINT_IMAGES(n)                                                       \
    {&objects.level_point_##n##__level_image, "S:/assets/level-image.jpg"},         \
    {&objects.level_point_##n##__level_done_image, "S:/assets/level-done-image.jpg"}, \
    {&objects.level_point_##n##__level_ongoing_image, "S:/assets/level-ongoing-image.jpg"}

// "S:" is LV_FS_POSIX_LETTER; LVGL resolves these paths under LV_FS_POSIX_PATH (/sdcard).
const Background backgrounds[] = {
    {&objects.homepage_background, "S:/assets/homepage-background.jpg"},
    {&objects.user_list_background, "S:/assets/user-list-background.jpg"},
    {&objects.chapter_background, "S:/assets/chapter-background.jpg"},
    {&objects.roadmap_background, "S:/assets/roadmap-background.jpg"},
    {&objects.profile_background, "S:/assets/profile-background.jpg"},
    {&objects.shop_background, "S:/assets/shop-background.jpg"},
    {&objects.overlay_buy_wear, "S:/assets/overlay-buy-wear.jpg"},
    // One nav bar per screen that has one; EEZ numbers these unnamed instances.
    {&objects.obj0__nav_bar, "S:/assets/nav-bar.jpg"},
    {&objects.obj1__nav_bar, "S:/assets/nav-bar.jpg"},
    {&objects.obj3__nav_bar, "S:/assets/nav-bar.jpg"},
    {&objects.obj6__nav_bar, "S:/assets/nav-bar.jpg"},
    {&objects.obj7__nav_bar, "S:/assets/nav-bar.jpg"},
    LEVEL_POINT_IMAGES(1),
    LEVEL_POINT_IMAGES(2),
    LEVEL_POINT_IMAGES(3),
    LEVEL_POINT_IMAGES(4),
    LEVEL_POINT_IMAGES(5),
    LEVEL_POINT_IMAGES(6),
    LEVEL_POINT_IMAGES(7),
    LEVEL_POINT_IMAGES(8),
};

uint32_t applied_mount_id = 0;

// The generated lookup only knows images[], which is empty in this project and
// dereferences a null name, so answer SD paths from flows here.
const void *find_flow_image(const char *name)
{
    return strncmp(name, "S:/", 3) == 0 ? name : nullptr;
}
}

void screen_backgrounds_init()
{
    eez::flow::getLvglImageByNameHook = find_flow_image;
}

void screen_backgrounds_tick()
{
    const uint32_t mount_id = sd_service_mount_id();
    if (mount_id == applied_mount_id) {
        return;
    }
    applied_mount_id = mount_id;

    for (const Background &background : backgrounds) {
        // Without a mounted card, clear the source so LVGL stops reading from it.
        lv_image_set_src(*background.widget, mount_id ? background.path : nullptr);
        if (mount_id && !lv_image_get_src(*background.widget)) {
            Serial.printf("[UI] Could not load background %s\n", background.path);
        }
    }
}
