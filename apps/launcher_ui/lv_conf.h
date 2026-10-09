/* Keep the launcher PAX on exactly the same LVGL configuration as the
 * validated LVGL demo while it is being migrated screen by screen. */
#include "../lvgl/lv_conf.h"

/* The compact launcher app counter uses this extra small face. */
#undef LV_FONT_MONTSERRAT_12
#define LV_FONT_MONTSERRAT_12 1
