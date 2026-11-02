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

// "S:" is LV_FS_POSIX_LETTER; LVGL resolves these paths under LV_FS_POSIX_PATH (/sdcard).
const Background backgrounds[] = {
    {&objects.background_homepage, "S:/homepage/homepage-background.jpg"},
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
