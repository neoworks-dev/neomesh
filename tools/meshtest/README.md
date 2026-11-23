# meshtest

Two-device test TUI for neomesh. Both boards plug into this computer over USB.
meshtest builds and flashes the firmware with PlatformIO, shows each board's
serial log in its own pane, and embeds the meshctl light-control protocol so you
can fire `led/color` commands at either board (or broadcast) from the UI.

```
neomesh test harness
 env: led_node   A: /dev/ttyACM0   B: /dev/ttyACM1   ● monitoring
╭ A  /dev/ttyACM0  A4CF1234ABCD0000 ─╮╭ B  /dev/ttyACM1  0011223344556677 ─╮
│ [   3.118] [MESH] node id: A4CF…  ││ [   3.120] [GW] GATEWAY ACTIVE      │
│ [   3.500] [APP] gateway elected  ││ [   3.540] [GW] IP:  192.168.1.50   │
╰────────────────────────────────────╯╰────────────────────────────────────╯
Lights  target: broadcast   color: ██ warm   bright: 128   gw: 192.168.1.50:7778 via log:B
• sent warm b=128 → broadcast via 192.168.1.50:7778
tab field  ←/→ change  ⏎/g send light  b build  1/2/f flash  m mon  d discover  …
```

## Run

```sh
cd tools/meshtest && go run . -C ../..
```

- `-C <dir>` — project root (the dir with `platformio.ini`); auto-detected otherwise.
- `-gw host:port` — gateway bridge address, skips discovery.

## Gateway address

Light commands go to the elected gateway's TcpBridge (port 7778). meshtest finds
it automatically:

1. **From the serial log** — when a board prints `[GW] IP: …` on becoming
   gateway, that address is captured. (Primary; needs that board monitored.)
2. **mDNS** — press `d` to browse `_neoworks._tcp`.
3. **`-gw` flag** — explicit override.

Each board's NodeID is sniffed from its `node id:` log line, so the `A` / `B`
light targets resolve without typing anything.

## Keys

| Key       | Action                                              |
|-----------|-----------------------------------------------------|
| `tab` / `shift+tab` | move focus across env / A / B / target / color / brightness |
| `←` `→`   | change the focused field                            |
| `⏎` or `g`| send the current colour to the selected target      |
| `b`       | build the selected env                              |
| `1` / `2` / `f` | flash device A / B / both                      |
| `m`       | start / stop monitoring both serial ports           |
| `d`       | discover the gateway via mDNS                       |
| `r`       | rescan serial ports                                 |
| `c`       | clear both log panes                                |
| `s`       | save both logs to `meshtest-<ts>.log`               |
| `↑/↓` `PgUp/PgDn` `Home/End` | scroll both panes (auto-follows at bottom) |
| `q`       | quit                                                |

Flashing a monitored device releases its port for the upload and resumes
monitoring when it reconnects (with a short retry while the C3 re-enumerates).
