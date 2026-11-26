"""
main.py — enkakumon touchscreen UI (docs/enkakumon ux.md): the Pet and Data
sides, fed by the daemon's JSON lines.

Usage: python src/ui/main.py [conf]   (run at boot by enkakumon-ui.service)

One loop, no threads: select() waits up to POLL_S for a line from the daemon
and touch is polled between waits. The bottom buttons switch Pet / Data; on
Data the top button cycles the metric; on Pet a tap is sent to the daemon
(it hatches a new pet if this one is dead).

Acceptance tests (on the Pi):
  the Pet side shows on start; buttons respond in under 1 s; the CO2 graph
  fills over time; restarting the daemon shows "daemon offline", then
  recovers with the outage drawn as a gap; SIGTERM blanks the screen and
  exits 0 (1 = config error, 2 = display init failed).
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

# (title, JSON key, unit, config key of the warn level)
METRICS = (
    ("CO₂", "co2", "ppm", "PET_CO2_MAX"),
    ("Air quality", "voc", "index", "PET_VOC_MAX"),
    ("Noise", "db", "dB SPL", "PET_DB_MAX"),
)
RETRY_S = 2.0   # reconnect interval while the daemon is unreachable
POLL_S = 0.05   # select timeout = touch polling interval
GAP_S = 2.5     # a longer silence is recorded as missing samples


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
                chunk = self.sock.recv(4096)
            except (BlockingIOError, InterruptedError):
                break
            except OSError:
                return None
            if not chunk:
                return None
            self._buf += chunk
        *lines, self._buf = self._buf.split(b"\n")
        return lines

    def send_tap(self):
        try:
            self.sock.send(b"tap\n")
        except OSError:
            pass  # a lost tap is harmless; read_lines notices a disconnect


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

    history = {key: collections.deque(maxlen=ui.HISTORY) for _, key, _, _ in METRICS}
    pet = {}
    client = DaemonClient(conf["SOCKET_PATH"])
    side, metric = "pet", 0
    live = False           # a line has arrived on this connection
    offline_shown = False
    next_connect = 0.0
    last_line_at = None    # kept across reconnects, so outages become gaps

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
                if live:  # a refusal (MAX_CLIENTS) before any data stays quiet
                    log("daemon disconnected")
                    offline_shown = False
                client.close()
                live = False
                next_connect = time.monotonic() + RETRY_S
                continue
            for line in lines:
                try:
                    data = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(data, dict):
                    continue
                if not live:
                    log(f"connected to {client.path}")
                    live = True
                now = time.monotonic()
                if last_line_at is not None and now - last_line_at > GAP_S:
                    for _ in range(min(ui.HISTORY, round(now - last_line_at) - 1)):
                        for h in history.values():
                            h.append(None)
                last_line_at = now
                for _, key, _, _ in METRICS:
                    history[key].append(number(data.get(key)))
                pet = data
                redraw = True

        tap = disp.get_tap()
        if tap is not None:
            x, y = tap
            if y >= ui.NAV_Y:
                side = "pet" if x < ui.WIDTH // 2 else "data"
            elif side == "data" and y < ui.TOP_H:
                metric = (metric + 1) % len(METRICS)
            elif side == "pet":
                client.send_tap()
            redraw = True

        if redraw and live:
            if side == "pet":
                disp.show(ui.render_pet(number(pet.get("health")), pet.get("state"), pet.get("task")))
            else:
                title, key, unit, warn_key = METRICS[metric]
                disp.show(ui.render_graph(title, unit, history[key], conf[warn_key]))

    client.close()
    disp.blank()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
