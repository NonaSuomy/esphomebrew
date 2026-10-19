# Clock synchronization after Wi-Fi changes

The P4 clock is synchronized through SNTP using `0.pool.ntp.org` and `1.pool.ntp.org`. The shared PAX profile enables two ESP-IDF SNTP server slots so both configured servers are usable. The display keeps its unset-time placeholder until the system clock is valid; the timezone is America/Toronto.

The Wi-Fi main loop detects each transition to a connected network with an IP address, including startup, Settings changes, and reconnection after an outage. It calls the ESP-IDF thread-safe SNTP restart wrapper once per transition, clearing the retry delay accumulated while offline. It does not restart SNTP repeatedly on every UI refresh. Successful synchronization emits an information-level `pax_clock` log with the received local date and time.

DNS resolution and outbound NTP (UDP port 123) still need to work on the selected network. A restart requests synchronization; it cannot manufacture correct time when the network blocks time servers. Logs distinguish `Wi-Fi connected; requesting time sync` from `Time synchronized: ...`.

Validation: host fixtures exercise initial connection, repeated connected polls, disconnect/reconnect, and builds without SNTP. The SD Wi-Fi backup tests also pass. Firmware configuration validation and an Elecrow build verify the configured server capacity and generated time-sync callback. On the Elecrow P4 AIO, the flashed firmware connected to Wi-Fi, immediately logged its sync request, and confirmed `Time synchronized: 2026-10-19 15:30:57` (America/Toronto) during startup. Serial flash contents were verified by esptool.
