TJpgDec R0.03 by ChaN, from LVGL commit 85aa60d18b3d5e5588d7b247abf90198f07c8a63, src/libs/tjpgd.

Local changes: replace the LVGL configuration-header dependency with LV_USE_TJPGD=1; select RGB565 output; restore RGB channel order before RGB565 conversion (the LVGL RGB888 path emits BGR). Copyright/license retained in source and LICENSE.txt.
