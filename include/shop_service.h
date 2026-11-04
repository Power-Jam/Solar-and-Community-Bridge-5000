#pragma once

/** Hook up the shop cards and the Buy & wear popup. Call after users_init(). */
void shop_init();
/** Show the dress-up figure's layers as they finish loading. Call from the LVGL task, after users_tick(). */
void shop_tick();
