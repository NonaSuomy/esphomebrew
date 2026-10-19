"""Exercise the production reconnect hook without Wi-Fi hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class WifiTimeSync(unittest.TestCase):
    def test_sync_once_per_connection_and_no_sntp_build(self):
        stream = (ROOT / 'components/pax_loader/pax_wifi_stream.h').read_text()
        hook = stream[stream.index('static void refresh_time_after_reconnect'):stream.index('static pax_wifi_status_t snapshot')]
        for enabled in (True, False):
            source = '#include <cassert>\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n'
            if enabled:
                source += '#define USE_ESP32\n#define SNTP_SERVER_COUNT 2\nstatic int restarts=0;static bool esp_sntp_restart(){restarts++;return true;}\n'
            source += hook + '''
int main(){
 refresh_time_after_reconnect(false);refresh_time_after_reconnect(false);
 refresh_time_after_reconnect(true);refresh_time_after_reconnect(true);
#ifdef USE_ESP32
 assert(restarts==1);
#endif
 refresh_time_after_reconnect(false);refresh_time_after_reconnect(true);
 refresh_time_after_reconnect(true);
#ifdef USE_ESP32
 assert(restarts==2);
#endif
}
'''
            with tempfile.TemporaryDirectory() as folder:
                cpp = Path(folder) / 'clock.cpp'; cpp.write_text(source)
                exe = Path(folder) / 'clock'
                subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()
