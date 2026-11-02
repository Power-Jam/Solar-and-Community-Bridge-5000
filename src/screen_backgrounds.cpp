#include "screen_backgrounds.h"

#include "eez-flow.h"
#include "screens.h"
#include "sd_service.h"

#include <cstdlib>
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
// Files load in this order, so what the user sees first belongs at the top.
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

constexpr size_t MaxFiles = sizeof(backgrounds) / sizeof(backgrounds[0]);

struct LoadedFile {
    const char *path;
    uint16_t *pixels;
    lv_image_dsc_t descriptor;
};

// One entry per distinct file: widgets showing the same file share its pixels.
LoadedFile files[MaxFiles];
const char *file_paths[MaxFiles];
size_t file_count = 0;

LoadedFile *find_file(const char *path)
{
    for (size_t i = 0; i < file_count; ++i) {
        if (strcmp(files[i].path, path) == 0) {
            return &files[i];
        }
    }
    return nullptr;
}

// The generated lookup only knows images[], which is empty in this project and
// dereferences a null name, so answer SD paths from flows here. Files that are
// not in PSRAM (yet) fall back to LVGL decoding them from the card.
const void *find_flow_image(const char *name)
{
    if (strncmp(name, "S:/", 3) != 0) {
        return nullptr;
    }
    const LoadedFile *file = find_file(name);
    if (file && file->pixels) {
        return &file->descriptor;
    }
    return name;
}

void apply_loaded_file(const SdBackground &loaded)
{
    LoadedFile &file = files[loaded.index];
    uint16_t *old_pixels = file.pixels;

    file.pixels = loaded.image.pixels;
    file.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    file.descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    file.descriptor.header.flags = 0;
    file.descriptor.header.w = loaded.image.width;
    file.descriptor.header.h = loaded.image.height;
    file.descriptor.header.stride = loaded.image.width * sizeof(uint16_t);
    file.descriptor.data_size = static_cast<uint32_t>(
        loaded.image.width * loaded.image.height * sizeof(uint16_t));
    file.descriptor.data = reinterpret_cast<const uint8_t *>(file.pixels);

    for (const Background &background : backgrounds) {
        if (strcmp(background.path, file.path) == 0) {
            lv_image_set_src(*background.widget, &file.descriptor);
        }
    }
    // Flows can show this descriptor on widgets outside the table.
    lv_obj_invalidate(lv_screen_active());

    // LVGL draws on this task, so nothing is still reading the old pixels.
    free(old_pixels);
}
}

void screen_backgrounds_init()
{
    eez::flow::getLvglImageByNameHook = find_flow_image;

    for (const Background &background : backgrounds) {
        if (!find_file(background.path)) {
            files[file_count].path = background.path;
            // The SD service reads through the VFS, which has no "S:" drive letter.
            file_paths[file_count] = background.path + 2;
            ++file_count;
        }
    }
    sd_service_set_backgrounds(file_paths, file_count);
}

void screen_backgrounds_tick()
{
    SdBackground loaded;
    while (sd_service_take_background(&loaded)) {
        apply_loaded_file(loaded);
    }
}
