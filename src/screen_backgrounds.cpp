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

// JPGs have no transparency, so each level has its own crop that includes the road behind it.
#define LEVEL_POINT_IMAGES(n)                                                       \
    {&objects.level_point_##n##__level_image, "S:/assets/level-" #n ".jpg"},         \
    {&objects.level_point_##n##__level_done_image, "S:/assets/level-" #n "-done.jpg"}, \
    {&objects.level_point_##n##__level_ongoing_image, "S:/assets/level-" #n "-ongoing.jpg"}

#define USER_ROW_ARROW(n) {&objects.user_row_##n##_arrow, "S:/assets/arrow-row.jpg"}

// Every avatar in every size a screen shows it; users_service picks them per learner.
#define AVATAR_FILES(n)                                                                  "S:/assets/avatars/avatar-" #n "-210.jpg", "S:/assets/avatars/avatar-" #n "-90.jpg",     "S:/assets/avatars/avatar-" #n "-42.jpg", "S:/assets/avatars/avatar-" #n "-78.jpg",      "S:/assets/avatars/avatar-" #n "-86.jpg", "S:/assets/avatars/avatar-" #n "-nav.jpg"

#define SHOP_ITEM_FILE(n) "S:/assets/shop/item-" #n ".jpg"

// "S:" is LV_FS_POSIX_LETTER; LVGL resolves these paths under LV_FS_POSIX_PATH (/sdcard).
// Files load in this order, so what the user sees first belongs at the top.
const Background backgrounds[] = {
    {&objects.homepage_background, "S:/assets/homepage-background.jpg"},
    {&objects.user_list_background, "S:/assets/user-list-background.jpg"},
    {&objects.user_creation_background, "S:/assets/user-creation-background.jpg"},
    {&objects.chapter_background, "S:/assets/chapter-background.jpg"},
    {&objects.roadmap_background, "S:/assets/roadmap-background.jpg"},
    {&objects.profile_background, "S:/assets/profile-background.jpg"},
    {&objects.shop_background, "S:/assets/shop-background.jpg"},
    {&objects.overlay_buy_wear, "S:/assets/overlay-buy-wear.jpg"},
    // One nav bar per screen that has one; the title is part of the image.
    {&objects.nav_bar_chapters__nav_bar, "S:/assets/nav-bar-chapters.jpg"},
    {&objects.nav_bar_roadmap__nav_bar, "S:/assets/nav-bar.jpg"},
    {&objects.nav_bar_level__nav_bar, "S:/assets/nav-bar.jpg"},
    {&objects.nav_bar_profile__nav_bar, "S:/assets/nav-bar-profile.jpg"},
    {&objects.nav_bar_shop__nav_bar, "S:/assets/nav-bar-shop.jpg"},
    LEVEL_POINT_IMAGES(1),
    LEVEL_POINT_IMAGES(2),
    LEVEL_POINT_IMAGES(3),
    LEVEL_POINT_IMAGES(4),
    LEVEL_POINT_IMAGES(5),
    LEVEL_POINT_IMAGES(6),
    LEVEL_POINT_IMAGES(7),
    LEVEL_POINT_IMAGES(8),
    {&objects.home_arrow_prev, "S:/assets/arrow-back.jpg"},
    {&objects.home_arrow_next, "S:/assets/arrow-next.jpg"},
    USER_ROW_ARROW(1),
    USER_ROW_ARROW(2),
    USER_ROW_ARROW(3),
    USER_ROW_ARROW(4),
    USER_ROW_ARROW(5),
    USER_ROW_ARROW(6),
    USER_ROW_ARROW(7),
    USER_ROW_ARROW(8),
    USER_ROW_ARROW(9),
    USER_ROW_ARROW(10),
    USER_ROW_ARROW(11),
    USER_ROW_ARROW(12),
};

// Loaded after the backgrounds, without a fixed widget.
const char *const preloaded[] = {
    AVATAR_FILES(1), AVATAR_FILES(2), AVATAR_FILES(3), AVATAR_FILES(4), AVATAR_FILES(5),
    AVATAR_FILES(6), AVATAR_FILES(7), AVATAR_FILES(8), AVATAR_FILES(9),
    // Shop popup: shop_service picks the selected item.
    SHOP_ITEM_FILE(1), SHOP_ITEM_FILE(2), SHOP_ITEM_FILE(3), SHOP_ITEM_FILE(4),
    SHOP_ITEM_FILE(5), SHOP_ITEM_FILE(6), SHOP_ITEM_FILE(7), SHOP_ITEM_FILE(8),
    SHOP_ITEM_FILE(9), SHOP_ITEM_FILE(10), SHOP_ITEM_FILE(11), SHOP_ITEM_FILE(12),
    "S:/assets/shop/icon-buy.jpg", "S:/assets/shop/icon-wear.jpg",
};

constexpr size_t MaxFiles =
    sizeof(backgrounds) / sizeof(backgrounds[0]) + sizeof(preloaded) / sizeof(preloaded[0]);

struct LoadedFile {
    const char *path;
    uint16_t *pixels;
    lv_image_dsc_t descriptor;
};

// One entry per distinct file: widgets showing the same file share its pixels.
LoadedFile files[MaxFiles];
const char *file_paths[MaxFiles];
size_t file_count = 0;
uint32_t loaded_version = 0;

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

void add_file(const char *path)
{
    if (!find_file(path)) {
        files[file_count].path = path;
        // The SD service reads through the VFS, which has no "S:" drive letter.
        file_paths[file_count] = path + 2;
        ++file_count;
    }
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
    ++loaded_version;
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
        add_file(background.path);
    }
    for (const char *path : preloaded) {
        add_file(path);
    }
    sd_service_set_backgrounds(file_paths, file_count);
}

const void *screen_backgrounds_image(const char *path)
{
    const LoadedFile *file = find_file(path);
    return file && file->pixels ? &file->descriptor : nullptr;
}

uint32_t screen_backgrounds_version()
{
    return loaded_version;
}

void screen_backgrounds_tick()
{
    SdBackground loaded;
    while (sd_service_take_background(&loaded)) {
        apply_loaded_file(loaded);
    }
}
