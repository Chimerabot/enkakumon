"""
main.py — enkakumon touchscreen UI: graphs of the daemon's readings.

Usage (from the activated venv): python src/ui/main.py [conf]

One loop, no threads: select() waits up to POLL_S for a JSON line from the
daemon, and touch is polled between waits. Each line is appended to a
600-sample history per metric and the current screen is redrawn; a tap
cycles the screen and sends "tap\\n" to the daemon.

Acceptance tests (on the Pi):
  with the daemon running, the CO2 graph fills over time; taps cycle
  CO2 -> Air quality -> Noise -> CO2 and respond in under 1 s; the screen
  updates every second without flicker; restarting the daemon shows
  "daemon offline" and then recovers. Ctrl-C or SIGTERM blanks the screen
  and exits 0.
"""
import collections
import json
import select
import signal
import socket
import sys
import time

import conf as confmod
from display import HISTORY, Display

# (title, JSON key, unit, warn level), in tap order
SCREENS = (
    ("CO₂", "co2", "ppm", 1000),
    ("Air quality", "voc", "index", 150),
    ("Noise", "db", "dB SPL", 70),
)
RETRY_S = 2.0      # reconnect interval while the daemon is unreachable
POLL_S = 0.05      # select timeout = touch polling interval
MAX_LINE = 4096    # longer "lines" are garbage; drop them


def log(msg):
    print(f"enkakumon-ui: {msg}", file=sys.stderr, flush=True)


class DaemonClient:
    """Non-blocking connection to the daemon's Unix socket."""

    def __init__(self, path):
        self.path = path
        self.sock = None
        self._buf = b""

    def connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)  # non-inheritable by default
        s.setblocking(False)
        try:
            s.connect(self.path)
        except OSError:
            s.close()
            return False
        self.sock = s
        self._buf = b""
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
        if len(self._buf) > MAX_LINE:
            log("dropping an over-long line from the daemon")
            self._buf = b""
        return lines

    def send_tap(self):
        try:
            self.sock.send(b"tap\n")
        except (BlockingIOError, InterruptedError):
            pass  # the daemon is behind; a lost tap is harmless
        except OSError:
            pass  # read_lines will notice the disconnect


def parse_reading(line):
    """Return {key: number or None} for the graphed keys, or None if malformed."""
    try:
        data = json.loads(line)
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    out = {}
    for _, key, _, _ in SCREENS:
        v = data.get(key)
        out[key] = v if isinstance(v, (int, float)) and not isinstance(v, bool) else None
    return out


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
        disp = Display(conf)
    except Exception as e:  # any driver/pin/SPI problem: nothing to show it on
        log(f"display init failed: {e}")
        return 2

    history = {key: collections.deque(maxlen=HISTORY) for _, key, _, _ in SCREENS}
    client = DaemonClient(conf["SOCKET_PATH"])
    screen = 0
    next_connect = 0.0
    offline_shown = False
    bad_lines = 0
    redraw = True

    while not stop:
        now = time.monotonic()
        if client.sock is None and now >= next_connect:
            if client.connect():
                log(f"connected to {client.path}")
                offline_shown = False
                redraw = True
            else:
                next_connect = now + RETRY_S

        if client.sock is None:
            if not offline_shown:
                disp.show_message("daemon offline")
                offline_shown = True
            if disp.get_tap() is not None:
                screen = (screen + 1) % len(SCREENS)
            time.sleep(POLL_S)
            continue

        readable, _, _ = select.select([client.sock], [], [], POLL_S)
        if readable:
            lines = client.read_lines()
            if lines is None:
                log("daemon disconnected")
                client.close()
                next_connect = time.monotonic() + RETRY_S
                continue
            for line in lines:
                reading = parse_reading(line)
                if reading is None:
                    bad_lines += 1
                    if bad_lines == 1 or bad_lines % 100 == 0:
                        log(f"ignored {bad_lines} malformed line(s)")
                    continue
                for key, value in reading.items():
                    history[key].append(value)
                redraw = True

        if disp.get_tap() is not None:
            screen = (screen + 1) % len(SCREENS)
            client.send_tap()
            redraw = True

        if redraw:
            title, key, unit, warn = SCREENS[screen]
            disp.draw_graph(title, unit, history[key], warn)
            redraw = False

    client.close()
    disp.blank()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
