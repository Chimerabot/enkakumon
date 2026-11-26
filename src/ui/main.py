"""
main.py — enkakumon touchscreen UI (docs/enkakumon ux.md): the Pet and Data
sides, fed by the daemon's JSON lines.

Usage: python src/ui/main.py [conf]   (run at boot by enkakumon-ui.service)

One loop, no threads: select() waits up to POLL_S for a line from the daemon
and touch is polled between waits. The bottom buttons switch Pet / Data. On
Data, the top-left button cycles the screen (four metrics, then "other") and
the top-right one the period. The 10-minute view is drawn from the live
lines; longer periods are asked of the daemon ("history <minutes>") and
refreshed each minute. On Pet, a tap is sent to the daemon (it hatches a new
pet if this one is dead). Any health change pops up for POPUP_S.

Acceptance tests (on the Pi):
  the Pet side shows on start; buttons respond in under 1 s; the CO2 graph
  fills over time; the 1 h to 48 h periods show stored history, with a
  dashed grey line where the device was off; restarting the daemon shows
  "daemon offline", then recovers with the outage drawn as a gap; SIGTERM
  blanks the screen and exits 0 (1 = config error, 2 = display init failed).
"""
import collections
import json
import select
import signal
import socket
import sys
import time

import conf as confmod
import display as ui

# Graph screens: (title, JSON key, unit, config key of the warn level, name color)
METRICS = (
    ("CO₂", "co2", "ppm", "PET_CO2_MAX", ui.BLUE),
    ("Temperature", "temp_c", "C", "PET_TEMP_MAX", ui.DARK_RED),
    ("Air quality", "voc", "index", "PET_VOC_MAX", ui.GREEN),
    ("Noise", "db", "dB SPL", "PET_DB_MAX", ui.PURPLE),
)
OTHER = len(METRICS)  # the last Data screen
PERIODS = ((10, "10m"), (60, "1h"), (720, "12h"), (1440, "24h"), (2880, "48h"))
LIVE = 600          # 10 minutes of 1 Hz samples
RETRY_S = 2.0       # reconnect interval while the daemon is unreachable
POLL_S = 0.05       # select timeout = touch polling interval
GAP_S = 2.5         # a longer silence is recorded as missing samples
REFRESH_S = 60.0    # history gains one record a minute
ASK_AGAIN_S = 5.0   # re-ask if a history request got no answer
POPUP_S = 2.0


def log(msg):
    print(f"enkakumon-ui: {msg}", file=sys.stderr, flush=True)


def number(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else None


class DaemonClient:
    """Non-blocking connection to the daemon's Unix socket."""

    def __init__(self, path):
        self.path, self.sock, self._buf = path, None, b""

    def connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.setblocking(False)
        try:
            s.connect(self.path)
        except OSError:
            s.close()
            return False
        self.sock, self._buf = s, b""
        return True

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def read_lines(self):
        """Complete lines received so far, or None if the daemon went away."""
        while True:
            try:
                chunk = self.sock.recv(16384)
            except (BlockingIOError, InterruptedError):
                break
            except OSError:
                return None
            if not chunk:
                return None
            self._buf += chunk
        *lines, self._buf = self._buf.split(b"\n")
        return lines

    def send(self, text):
        try:
            self.sock.send(text.encode())
        except OSError:
            pass  # a lost tap or request is harmless; read_lines notices a disconnect


def recent_steps(counts):
    """Steps in the live window; the counter can restart at 0 but never counts down."""
    total, last = 0, None
    for c in counts:
        if c is not None:
            if last is not None and c > last:
                total += c - last
            last = c
    return total


def main(argv):
    try:
        conf = confmod.load(argv[1] if len(argv) > 1 else confmod.DEFAULT_PATH)
    except confmod.ConfError as e:
        log(str(e))
        return 1

    stop = False

    def on_signal(signum, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    try:
        disp = ui.Display(conf)
    except Exception as e:  # any driver/pin/SPI problem: nothing to show it on
        log(f"display init failed: {e}")
        return 2

    live = {key: collections.deque(maxlen=LIVE) for key in ("co2", "temp_c", "voc", "db", "steps")}
    latest = {}            # the newest live line
    hist = {}              # the newest history answer
    hist_at = asked_at = -1e9
    client = DaemonClient(conf["SOCKET_PATH"])
    side, screen, period = "pet", 0, 0
    connected = False      # a line has arrived on this connection
    offline_shown = False
    next_connect = 0.0
    last_line_at = None    # kept across reconnects, so outages become gaps
    popup, popup_until = 0, 0.0

    while not stop:
        if client.sock is None and time.monotonic() >= next_connect and not client.connect():
            next_connect = time.monotonic() + RETRY_S
        if client.sock is None:
            if not offline_shown:
                disp.show(ui.render_message("daemon offline"))
                offline_shown = True
            time.sleep(POLL_S)
            continue

        redraw = False
        if select.select([client.sock], [], [], POLL_S)[0]:
            lines = client.read_lines()
            if lines is None:
                if connected:  # a refusal (MAX_CLIENTS) before any data stays quiet
                    log("daemon disconnected")
                    offline_shown = False
                client.close()
                connected = False
                next_connect = time.monotonic() + RETRY_S
                continue
            for line in lines:
                try:
                    data = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(data, dict):
                    continue
                now = time.monotonic()
                if "hist" in data:
                    hist, hist_at = data, now
                    redraw = True
                    continue
                if not connected:
                    log(f"connected to {client.path}")
                    connected = True
                if last_line_at is not None and now - last_line_at > GAP_S:
                    for _ in range(min(LIVE, round(now - last_line_at) - 1)):
                        for q in live.values():
                            q.append(None)
                last_line_at = now
                for key, q in live.items():
                    q.append(number(data.get(key)))
                old, new = number(latest.get("health")), number(data.get("health"))
                if old is not None and new is not None and new != old:
                    popup, popup_until = new - old, now + POPUP_S
                latest = data
                redraw = True

        now = time.monotonic()
        if popup and now >= popup_until:
            popup, redraw = 0, True

        tap = disp.get_tap()
        if tap is not None:
            x, y = tap
            if y >= ui.NAV_Y:
                side = "pet" if x < ui.WIDTH // 2 else "data"
            elif side == "data" and y < ui.TOP_H:
                if x >= ui.PERIOD_X and screen != OTHER:
                    period = (period + 1) % len(PERIODS)
                else:
                    screen = (screen + 1) % (len(METRICS) + 1)
            elif side == "pet":
                client.send("tap\n")
            redraw = True

        minutes, label = PERIODS[period]
        wants_history = side == "data" and screen != OTHER and minutes > PERIODS[0][0]
        if wants_history and (hist.get("hist") != minutes or now - hist_at >= REFRESH_S) \
                and now - asked_at >= ASK_AGAIN_S:
            client.send(f"history {minutes}\n")
            asked_at = now

        if not (redraw and connected):
            continue
        if side == "pet":
            img = ui.render_pet(number(latest.get("health")), latest.get("state"), latest.get("task"))
        elif screen == OTHER:
            rgb = tuple(number(latest.get(k)) for k in ("red", "green", "blue"))
            img = ui.render_other(rgb, number(latest.get("proximity")),
                                  recent_steps(live["steps"]), number(latest.get("light")))
        else:
            title, key, unit, warn_key, color = METRICS[screen]
            current = live[key][-1] if live[key] else None
            if minutes == PERIODS[0][0]:
                values, slots, skips = list(live[key]), LIVE, ()
            elif hist.get("hist") == minutes:
                values = [number(v) for v in hist.get(key, [])]
                slots, skips = len(values), hist.get("skip", ())
            else:
                values, slots, skips = [], 1, ()  # waiting for the daemon's answer
            img = ui.render_graph(title, color, unit, values, slots, current,
                                  conf[warn_key], label, skips)
        if popup:
            ui.draw_popup(img, popup)
        disp.show(img)

    client.close()
    disp.blank()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
