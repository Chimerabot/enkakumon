"""
display.py — ILI9341 screen (portrait 240x320) and TSC2007 touch controller.

Frames are drawn off-screen with Pillow and pushed in one disp.image() call,
so the screen is never cleared mid-frame (no flicker). render_graph() and
render_message() need no hardware.

Run on its own to calibrate touch: python src/ui/display.py prints raw x/y
and the extremes seen; copy them into TOUCH_* in enkakumon.conf.

Acceptance tests (on the Pi):
  draw_graph shows the title, the current value in large text (white, orange
  above warn, red above 1.5x warn), up to 600 samples as a line graph with
  min/max labels and a dashed warn line; None values leave gaps, and a None
  latest value shows missing_text ("sensor offline" unless the caller says
  otherwise, e.g. "warming up").
  get_tap returns one (x, y) in screen pixels per press, however long the
  press; touching the top-left corner gives roughly (0, 0).
  An unplugged touch controller is logged once and retried every RETRY_S.
"""
import sys
import time

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 240, 320
HISTORY = 600  # samples per graph: 10 minutes at 1 Hz

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
GREY = (120, 120, 120)
ORANGE = (255, 165, 0)
RED = (255, 48, 48)

# Graph area
GX0, GX1 = 44, 232
GY0, GY1 = 100, 292

RELEASE_READS = 3  # consecutive unpressed reads (~150 ms) that end a press

_FONT_PATHS = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   # Debian / Raspberry Pi OS
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",  # Fedora / RHEL
)
_fonts = {}
_fallback_font = False


def _font(size):
    global _fallback_font
    if size not in _fonts:
        for path in _FONT_PATHS:
            try:
                _fonts[size] = ImageFont.truetype(path, size)
                break
            except OSError:
                continue
        else:
            _fonts[size] = ImageFont.load_default()
            _fallback_font = True
    return _fonts[size]


def _text(text):
    # The built-in bitmap font has no subscript digits.
    return text.replace("₂", "2") if _fallback_font else text


def _log(msg):
    print(f"enkakumon-ui: {msg}", file=sys.stderr, flush=True)


def value_color(value, warn):
    if value > 1.5 * warn:
        return RED
    if value > warn:
        return ORANGE
    return WHITE


def format_value(value):
    if abs(value) >= 100 or float(value).is_integer():
        return f"{value:.0f}"
    return f"{value:.1f}"


def render_graph(title, unit, values, warn_level, missing_text="sensor offline"):
    """Return a WIDTH x HEIGHT image of one metric's graph."""
    img = Image.new("RGB", (WIDTH, HEIGHT), BLACK)
    d = ImageDraw.Draw(img)
    small, title_font, big = _font(12), _font(18), _font(44)

    values = list(values)[-HISTORY:]
    latest = values[-1] if values else None
    d.text((8, 6), _text(title), font=title_font, fill=WHITE)
    if latest is not None:
        text = format_value(latest)
        d.text((8, 32), text, font=big, fill=value_color(latest, warn_level))
        d.text((12 + d.textlength(text, font=big), 58), unit, font=_font(16), fill=GREY)
    else:
        d.text((8, 46), "no data yet" if not values else missing_text,
               font=_font(22), fill=GREY)

    # Y range covers the data and the warn level, so the warn line is always shown.
    nums = [v for v in values if v is not None]
    lo = min(nums + [warn_level]) if nums else 0
    hi = max(nums + [warn_level])
    if hi == lo:
        lo, hi = lo - 1, hi + 1
    pad = (hi - lo) * 0.05
    lo, hi = (lo - pad if lo < 0 else max(0, lo - pad)), hi + pad

    def ypos(v):
        return GY1 - (v - lo) / (hi - lo) * (GY1 - GY0)

    # Right-aligned: the newest sample is always at "now".
    offset = HISTORY - len(values)

    def xpos(i):
        return GX0 + (offset + i) * (GX1 - GX0) / (HISTORY - 1)

    d.line([(GX0, GY0), (GX0, GY1), (GX1, GY1)], fill=GREY)
    decimals = 0 if hi - lo >= 10 else 1
    for label, y in ((f"{hi:.{decimals}f}", GY0), (f"{lo:.{decimals}f}", GY1)):
        d.text((GX0 - 4 - d.textlength(label, font=small), y - 7), label, font=small, fill=GREY)
    d.text((GX0, GY1 + 6), "-10 min", font=small, fill=GREY)
    d.text((GX1 - d.textlength("now", font=small), GY1 + 6), "now", font=small, fill=GREY)

    yw = ypos(warn_level)
    for x in range(GX0 + 2, GX1, 8):
        d.line([(x, yw), (min(x + 4, GX1), yw)], fill=ORANGE)

    # Consecutive non-None samples form one polyline; None breaks the line.
    run = []
    for i, v in enumerate(values + [None]):
        if v is not None:
            run.append((xpos(i), ypos(v)))
            continue
        if len(run) == 1:
            d.point(run, fill=WHITE)
        elif run:
            d.line(run, fill=WHITE)
        run = []
    return img


def render_message(text):
    img = Image.new("RGB", (WIDTH, HEIGHT), BLACK)
    d = ImageDraw.Draw(img)
    font = _font(22)
    d.text(((WIDTH - d.textlength(text, font=font)) / 2, HEIGHT / 2 - 12),
           text, font=font, fill=WHITE)
    return img


def map_touch(raw_x, raw_y, conf):
    """Map raw 12-bit touch readings to screen pixels using the TOUCH_* keys."""
    if conf.get("TOUCH_SWAP_XY", 0):
        raw_x, raw_y = raw_y, raw_x

    def scale(raw, lo, hi, size):
        if hi == lo:
            return 0
        pos = round((raw - lo) * (size - 1) / (hi - lo))
        return max(0, min(size - 1, pos))

    return (scale(raw_x, conf["TOUCH_X_MIN"], conf["TOUCH_X_MAX"], WIDTH),
            scale(raw_y, conf["TOUCH_Y_MIN"], conf["TOUCH_Y_MAX"], HEIGHT))


class Display:
    def __init__(self, conf):
        import board
        import digitalio
        from adafruit_rgb_display import ili9341

        self._conf = conf
        self._retry_s = conf.get("RETRY_S", 30)
        self._pressure_min = conf.get("TOUCH_PRESSURE_MIN", 100)
        self._disp = ili9341.ILI9341(
            board.SPI(),
            cs=digitalio.DigitalInOut(board.CE0),
            dc=digitalio.DigitalInOut(getattr(board, f"D{conf['DC_PIN']}")),
            rst=digitalio.DigitalInOut(getattr(board, f"D{conf['RST_PIN']}")),
            baudrate=conf.get("SPI_BAUD", 24000000),
            width=WIDTH, height=HEIGHT, rotation=0)
        self._i2c = board.I2C()
        self._touch = None
        self._touch_logged = False
        self._touch_retry_at = 0.0
        self._pressed = False
        self._release_reads = 0

    def show(self, img):
        self._disp.image(img)

    def draw_graph(self, title, unit, values, warn_level, missing_text="sensor offline"):
        self.show(render_graph(title, unit, values, warn_level, missing_text))

    def show_message(self, text):
        self.show(render_message(text))

    def blank(self):
        self.show(Image.new("RGB", (WIDTH, HEIGHT), BLACK))

    def _touch_failed(self, err):
        if not self._touch_logged:
            _log(f"touch: {err} (retrying every {self._retry_s} s)")
            self._touch_logged = True
        self._touch = None
        self._pressed = False
        self._touch_retry_at = time.monotonic() + self._retry_s

    def _touch_ready(self):
        if self._touch is not None:
            return True
        if time.monotonic() < self._touch_retry_at:
            return False
        import adafruit_tsc2007
        try:
            self._touch = adafruit_tsc2007.TSC2007(self._i2c)
        except (OSError, ValueError) as e:
            self._touch_failed(e)
            return False
        if self._touch_logged:
            _log("touch: recovered")
            self._touch_logged = False
        return True

    def get_tap(self):
        """One (x, y) per press, or None. A press ends after RELEASE_READS
        consecutive unpressed reads, so a wobbly finger is not two taps."""
        if not self._touch_ready():
            return None
        try:
            p = self._touch.touch
        except (OSError, ValueError) as e:
            self._touch_failed(e)
            return None

        if p["pressure"] > self._pressure_min:
            self._release_reads = 0
            if not self._pressed:
                self._pressed = True
                return map_touch(p["x"], p["y"], self._conf)
        elif self._pressed:
            self._release_reads += 1
            if self._release_reads >= RELEASE_READS:
                self._pressed = False
        return None


def _calibrate():
    import board
    import adafruit_tsc2007

    import conf as confmod

    c = confmod.load(sys.argv[1] if len(sys.argv) > 1 else confmod.DEFAULT_PATH)
    touch = adafruit_tsc2007.TSC2007(board.I2C())
    pmin = c.get("TOUCH_PRESSURE_MIN", 100)
    seen = None
    print("Drag a stylus or fingertip into each corner and along the edges. Ctrl-C to stop.")
    try:
        while True:
            p = touch.touch
            if p["pressure"] > pmin:
                x, y = p["x"], p["y"]
                seen = (x, x, y, y) if seen is None else (
                    min(seen[0], x), max(seen[1], x), min(seen[2], y), max(seen[3], y))
                print(f"raw x={x:4d} y={y:4d} pressure={p['pressure']:4d}  "
                      f"-> pixel {map_touch(x, y, c)}   seen x {seen[0]}..{seen[1]} "
                      f"y {seen[2]}..{seen[3]}")
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    if seen:
        print("\nTop-left corner raw values go in TOUCH_X_MIN / TOUCH_Y_MIN, bottom-right in "
              "TOUCH_X_MAX / TOUCH_Y_MAX.\nIf left-right movement changed y instead of x, "
              "set TOUCH_SWAP_XY = 1.")


if __name__ == "__main__":
    _calibrate()
