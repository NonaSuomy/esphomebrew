#ifndef _GB_CONFIG_H
#define _GB_CONFIG_H

/* Preserve the core resource-table API while reading all game data from SD.
 * No generated game-bank headers are included in this build. */
#define use_lib_dataflash
#define use_lib_width 320
#define use_lib_height 200
#define use_lib_offset_x 0
#define use_lib_offset_y 0
#define use_lib_video 0
#define use_lib_log_serial

#endif
