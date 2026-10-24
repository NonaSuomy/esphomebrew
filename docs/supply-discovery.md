# Find a local PAX Supply server

Start the existing server as usual:

```sh
cd ~/code/pax
./pax-tools/serve-dev.sh 8000
```

Open **LAN** in Launcher UI. Automatic mode finds the server on the same local
network, uses its current IPv4 address and actual HTTP port, and caches the URL
on SD. Refreshing LAN discovers it again, so moving networks or changing a DHCP
address does not require typing the computer's address. The short discovery wait
runs on the cancellable I/O worker; navigation remains available.

In **Settings → PAX SUPPLY SERVER**, enter an HTTP(S) URL to select and save a
manual server. Enter `auto` to restore automatic discovery. A cached URL is used
when discovery receives no reply. If several servers advertise on the same
network, automatic mode uses the first reply; use manual mode to choose one.

The server listens for discovery on UDP 18732 and replies to the requesting
client with its HTTP port. It does not scan addresses or advertise file paths.
The launcher matches a per-request token before accepting a reply and obtains
the address from the UDP sender. HTTP continues to serve the configured directory.
Use `--no-discovery` with `serve_dev.py` to disable discovery. Discovery failure
leaves HTTP serving and manual URLs available. A server already running before
this update must be restarted to enable the responder.
