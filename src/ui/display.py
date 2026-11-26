"""
display.py — ILI9341 screen (portrait 240x320), TSC2007 touch, and the
screens of docs/enkakumon ux.md, drawn black on white in the Pirkkala font.

Frames are drawn off-screen with Pillow and pushed in one call (no flicker).
The render_* functions need no hardware.
Run on its own to calibrate touch: python src/ui/display.py.

Acceptance tests (on the Pi):
  Every screen has "pet" and "data" buttons at the bottom; the current one is
  filled black. Pet: the sprite frame for the state, a health bar (red at or
  below 50) and, during a surprise, its prompt (a swatch for color tasks).
  Data: the metric's name in its color (tap for the next screen) and the time
  period (tap for the next period) at the top; the current value (orange
  above warn, red above 1.5x warn); a line graph with a dashed orange warn
  line, gaps for missing data and dashed grey lines at time skips. "other"
  shows color (with a swatch), proximity, recent steps and light in four
  quadrants. A health change shows a red or green popup at the top left.
  get_tap returns one (x, y) in screen pixels per press.
"""
import os
import sys
import time

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 240, 320
TOP_H = 32       # Data: the screen and period buttons
PERIOD_X = 176   # Data: taps right of this change the period
NAV_Y = 288      # all: the pet / data buttons

BLACK, WHITE, GREY = (0, 0, 0), (255, 255, 255), (110, 110, 110)
ORANGE, RED, GREEN = (215, 110, 0), (200, 0, 0), (0, 150, 0)  # dark enough to read on white
BLUE, DARK_RED, PURPLE = (0, 70, 200), (140, 0, 0), (120, 0, 160)
TASK_COLORS = {"red": (220, 0, 0), "green": (0, 170, 0), "blue": (0, 60, 220)}
TASK_TEXT = {"walk": "take me for a walk!", "music": "play me some music!"}

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets")
STATES = ("idle", "sleeping", "unhealthy", "dead")  # sprite frame order
SPRITE_SIZE, SPRITE_SCALE = 100, 2

GX0, GX1, GY0, GY1 = 44, 232, 104, 252  # graph area
RELEASE_READS = 3  # consecutive unpressed reads (~150 ms) that end a press

_fonts = {}
_frames = []


def _font(size=16):
    """Pirkkala is a pixel font: crisp only at 16, 32 and 48 px."""
    if size not in _fonts:
        try:
            _fonts[size] = ImageFont.truetype(os.path.join(ASSETS, "pirkkala.ttf"), size)
        except OSError:
            _fonts[size] = ImageFont.load_default()
    return _fonts[size]


def _frame(state):
    if not _frames:
        sheet = Image.open(os.path.join(ASSETS, "enkakumon-sprite.png")).convert("RGBA")
        size = SPRITE_SIZE * SPRITE_SCALE
        _frames.extend(
            sheet.crop((i * SPRITE_SIZE, 0, (i + 1) * SPRITE_SIZE, SPRITE_SIZE))
            .resize((size, size), Image.NEAREST)
            for i in range(len(STATES)))
    return _frames[STATES.index(state)]


def _canvas():
    img = Image.new("RGB", (WIDTH, HEIGHT), WHITE)
    return img, ImageDraw.Draw(img)


def _center(d, y, text, fill=BLACK, x0=0, x1=WIDTH):
    d.text(((x0 + x1 - d.textlength(text, font=_font())) / 2, y), text, font=_font(), fill=fill)


def _fmt(v):
    return f"{v:.0f}" if abs(v) >= 100 or float(v).is_integer() else f"{v:.1f}"


def _nav(d, active):
    for i, name in enumerate(("pet", "data")):
        x0, x1 = i * WIDTH // 2, (i + 1) * WIDTH // 2 - 1
        on = name == active
        d.rectangle([x0, NAV_Y, x1, HEIGHT - 1], fill=BLACK if on else WHITE, outline=BLACK)
        _center(d, NAV_Y + 8, name, WHITE if on else BLACK, x0, x1)


def _top(d, title, color, period=None):
    d.text((8, 8), title.replace("₂", "2") + " >", font=_font(), fill=color)  # no subscripts in Pirkkala
    if period is not None:
        label = period + " >"
        d.text((WIDTH - 8 - d.textlength(label, font=_font()), 8), label, font=_font(), fill=BLACK)
    d.line([(0, TOP_H - 1), (WIDTH, TOP_H - 1)], fill=GREY)


def draw_popup(img, delta):
    """A health change at the top left: green if positive, red if negative."""
    d = ImageDraw.Draw(img)
    text = f"{delta:+d}"
    w = d.textlength(text, font=_font())
    d.rectangle([2, 2, 14 + w, 26], fill=GREEN if delta > 0 else RED)
    d.text((8, 6), text, font=_font(), fill=WHITE)
    return img


def render_pet(health, state, task):
    if state not in STATES or not isinstance(health, int):
        return render_message("no pet data")
    img, d = _canvas()
    if state == "dead":
        _center(d, 8, "tap to hatch")
    elif task in TASK_COLORS:
        _center(d, 8, "show me", x1=WIDTH - 40)
        d.rectangle([WIDTH - 56, 4, WIDTH - 32, 27], fill=TASK_COLORS[task])
    elif task in TASK_TEXT:
        _center(d, 8, TASK_TEXT[task])
    frame = _frame(state)
    img.paste(frame, ((WIDTH - frame.width) // 2, 36), frame)

    x0, x1, y0, y1 = 20, WIDTH - 20, 252, 268
    d.rectangle([x0, y0, x1, y1], outline=BLACK)
    if health > 0:
        d.rectangle([x0 + 2, y0 + 2, x0 + 2 + (x1 - x0 - 4) * min(health, 100) // 100, y1 - 2],
                    fill=RED if health <= 50 else BLACK)
    _nav(d, "pet")
    return img


def render_graph(title, color, unit, values, slots, latest, warn, period, skips=()):
    """values: up to `slots` samples (None = missing), evenly spaced over the
    period and right-aligned at "now"; skips: indices into values where the
    device was off; latest: the current reading shown in large text."""
    img, d = _canvas()
    small, big = _font(16), _font(48)
    _top(d, title, color, period)
    if latest is None:
        d.text((8, 52), "no data", font=small, fill=GREY)
    else:
        text = _fmt(latest)
        value_color = RED if latest > 1.5 * warn else ORANGE if latest > warn else BLACK
        d.text((8, 36), text, font=big, fill=value_color)
        d.text((14 + d.textlength(text, font=big), 66), unit, font=small, fill=GREY)

    # The y range covers the data and the warn level, so the warn line always shows.
    nums = [v for v in values if v is not None]
    lo, hi = min(nums + [warn]) if nums else 0, max(nums + [warn])
    if hi == lo:
        lo, hi = lo - 1, hi + 1
    pad = (hi - lo) * 0.05
    lo, hi = max(0, lo - pad) if lo >= 0 else lo - pad, hi + pad

    def x(i):
        return GX0 + (slots - len(values) + i) * (GX1 - GX0) / max(slots - 1, 1)

    def y(v):
        return GY1 - (v - lo) / (hi - lo) * (GY1 - GY0)

    d.line([(GX0, GY0), (GX0, GY1), (GX1, GY1)], fill=GREY)
    fmt = ".0f" if hi - lo >= 10 else ".1f"
    for label, ly in ((format(hi, fmt), GY0), (format(lo, fmt), GY1)):
        d.text((GX0 - 4 - d.textlength(label, font=small), ly - 8), label, font=small, fill=GREY)
    d.text((GX0, GY1 + 6), "-" + period, font=small, fill=GREY)
    d.text((GX1 - d.textlength("now", font=small), GY1 + 6), "now", font=small, fill=GREY)

    yw = y(warn)
    for px in range(GX0 + 2, GX1, 8):
        d.line([(px, yw), (min(px + 4, GX1), yw)], fill=ORANGE)
    for i in skips:  # the device was off just before this sample
        if 0 <= i < len(values):
            for py in range(GY0, GY1, 8):
                d.line([(x(i), py), (x(i), min(py + 4, GY1))], fill=GREY)

    run = []  # consecutive samples form one line; None breaks it
    for i, v in enumerate(list(values) + [None]):
        if v is not None:
            run.append((x(i), y(v)))
        elif run:
            d.line(run, fill=BLACK) if len(run) > 1 else d.point(run, fill=BLACK)
            run = []
    _nav(d, "data")
    return img


def render_other(rgb, proximity, steps, light):
    """Four quadrants: color (with a swatch), proximity, recent steps, light."""
    img, d = _canvas()
    _top(d, "other", BLACK)
    mid_x, mid_y = WIDTH // 2, (TOP_H + NAV_Y) // 2
    d.line([(mid_x, TOP_H), (mid_x, NAV_Y)], fill=GREY)
    d.line([(0, mid_y), (WIDTH, mid_y)], fill=GREY)
    quads = (("color", (0, TOP_H)), ("proximity", (mid_x, TOP_H)),
             ("steps 10m", (0, mid_y)), ("light", (mid_x, mid_y)))
    values = (None, proximity, steps, light)
    for (label, (qx, qy)), v in zip(quads, values):
        d.text((qx + 8, qy + 8), label, font=_font(), fill=GREY)
        if label == "color":
            continue
        d.text((qx + 8, qy + 48), "--" if v is None else _fmt(v), font=_font(32), fill=BLACK)

    qx, qy = 0, TOP_H
    if None in rgb:
        d.text((qx + 8, qy + 56), "--", font=_font(32), fill=BLACK)
    else:
        m = max(rgb) or 1  # the swatch shows the hue at full brightness
        d.rectangle([qx + 8, qy + 36, qx + 48, qy + 76],
                    fill=tuple(round(c * 255 / m) for c in rgb), outline=BLACK)
        for i, (name, c) in enumerate(zip("rgb", rgb)):
            d.text((qx + 56, qy + 32 + 16 * i), f"{name} {c:.0f}", font=_font(), fill=BLACK)
    _nav(d, "data")
    return img


def render_message(text):
    img, d = _canvas()
    _center(d, HEIGHT // 2 - 8, text)
    return img


def map_touch(raw_x, raw_y, conf):
    """Map raw 12-bit touch readings to screen pixels using the TOUCH_* keys."""
    if conf.get("TOUCH_SWAP_XY", 0):
        raw_x, raw_y = raw_y, raw_x

    def scale(raw, lo, hi, size):
        return 0 if hi == lo else max(0, min(size - 1, round((raw - lo) * (size - 1) / (hi - lo))))

    return (scale(raw_x, conf["TOUCH_X_MIN"], conf["TOUCH_X_MAX"], WIDTH),
            scale(raw_y, conf["TOUCH_Y_MIN"], conf["TOUCH_Y_MAX"], HEIGHT))


class Display:
    def __init__(self, conf):
        import board
        import digitalio
        from adafruit_rgb_display import ili9341

        self._conf = conf
        self._retry_s = conf.get("RETRY_S", 30)
        self._disp = ili9341.ILI9341(
            board.SPI(),
            cs=digitalio.DigitalInOut(board.CE0),
            dc=digitalio.DigitalInOut(getattr(board, f"D{conf['DC_PIN']}")),
            rst=digitalio.DigitalInOut(getattr(board, f"D{conf['RST_PIN']}")),
            baudrate=conf.get("SPI_BAUD", 24000000),
            width=WIDTH, height=HEIGHT, rotation=0)
        self._i2c = board.I2C()
        self._touch = None
        self._touch_retry_at = 0.0
        self._touch_logged = False
        self._pressed = False
        self._release_reads = 0

    def show(self, img):
        self._disp.image(img)

    def blank(self):
        self.show(Image.new("RGB", (WIDTH, HEIGHT), BLACK))  # screen off

    def get_tap(self):
        """One (x, y) per press, or None. A press ends after RELEASE_READS
        unpressed reads, so a wobbly finger is not two taps. A missing touch
        controller is logged once and retried every RETRY_S."""
        try:
            if self._touch is None:
                if time.monotonic() < self._touch_retry_at:
                    return None
                import adafruit_tsc2007
                self._touch = adafruit_tsc2007.TSC2007(self._i2c)
                if self._touch_logged:
                    print("enkakumon-ui: touch: recovered", file=sys.stderr, flush=True)
                    self._touch_logged = False
            p = self._touch.touch
        except (OSError, ValueError) as e:
            if not self._touch_logged:
                print(f"enkakumon-ui: touch: {e} (retrying every {self._retry_s} s)",
                      file=sys.stderr, flush=True)
                self._touch_logged = True
            self._touch, self._pressed = None, False
            self._touch_retry_at = time.monotonic() + self._retry_s
            return None

        if p["pressure"] > self._conf.get("TOUCH_PRESSURE_MIN", 100):
            self._release_reads = 0
            if not self._pressed:
                self._pressed = True
                return map_touch(p["x"], p["y"], self._conf)
        elif self._pressed:
            self._release_reads += 1
            self._pressed = self._release_reads < RELEASE_READS
        return None


def _calibrate():
    import board
    import adafruit_tsc2007

    import conf as confmod

    c = confmod.load(sys.argv[1] if len(sys.argv) > 1 else confmod.DEFAULT_PATH)
    touch = adafruit_tsc2007.TSC2007(board.I2C())
    seen = None
    print("Drag a fingertip into each corner and along the edges. Ctrl-C to stop.")
    try:
        while True:
            p = touch.touch
            if p["pressure"] > c.get("TOUCH_PRESSURE_MIN", 100):
                x, y = p["x"], p["y"]
                seen = (x, x, y, y) if seen is None else (
                    min(seen[0], x), max(seen[1], x), min(seen[2], y), max(seen[3], y))
                print(f"raw x={x:4d} y={y:4d} -> pixel {map_touch(x, y, c)}  "
                      f"seen x {seen[0]}..{seen[1]} y {seen[2]}..{seen[3]}")
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    if seen:
        print("\nTop-left raw values go in TOUCH_X_MIN / TOUCH_Y_MIN, bottom-right in "
              "TOUCH_X_MAX / TOUCH_Y_MAX. If left-right movement changed y, set TOUCH_SWAP_XY = 1.")


if __name__ == "__main__":
    _calibrate()
