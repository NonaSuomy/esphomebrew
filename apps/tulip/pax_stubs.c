// Platform hooks Tulip's shared code expects that have nothing behind them on
// the PAX loader. Each one says what is missing.
#include "pax_port.h"

#include <stdint.h>

// modtulip.c tulip.app_path(): Tulip Desktop's resources folder. The PAX
// keeps everything in its filesystem image, mounted at /.
char *get_tulip_home_path(void)
{
    static char root[] = "/";
    return root;
}
