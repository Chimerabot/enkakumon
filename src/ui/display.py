"""
display.py — ILI9341 screen (portrait 240x320), TSC2007 touch, and the two
screens of docs/enkakumon ux.md, drawn black on white in the Pirkkala font.

Frames are drawn off-screen with Pillow and pushed in one call (no flicker).
render_pet(), render_graph() and render_message() need no hardware.
Run on its own to calibrate touch: python src/ui/display.py.

Acceptance tests (on the Pi):
  Both screens have "pet" and "data" buttons at the bottom; the current one
  is filled black. The Pet screen shows the sprite frame for the state, a
  health bar (red at or below 50) and, during a surprise, its prompt (a color
  swatch for color tasks). The Data screen shows the metric's name (tap it for
  the next metric), its current value (orange above warn, red above 1.5x
  warn), and a line graph with a dashed orange warn line; gaps stay gaps.
  get_tap returns one (x, y) in screen pixels per press.
"""
import os
import sys
import time

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 240, 320
HISTORY = 600   # samples per graph: 10 minutes at 1 Hz
TOP_H = 32      # Data: the metric button
NAV_Y = 288     # both: the pet / data buttons

BLACK, WHITE, GREY = (0, 0, 0), (255, 255, 255), (110, 110, 110)
ORANGE, RED = (215, 110, 0), (200, 0, 0)  # dark enough to read on white
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


def _nav(d, active):
    for i, name in enumerate(("pet", "data")):
        x0, x1 = i * WIDTH // 2, (i + 1) * WIDTH // 2 - 1
        on = name == active
        d.rectangle([x0, NAV_Y, x1, HEIGHT - 1], fill=BLACK if on else WHITE, outline=BLACK)
        _center(d, NAV_Y + 8, name, WHITE if on else BLACK, x0, x1)


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


def _value_color(value, warn):
    return RED if value > 1.5 * warn else ORANGE if value > warn else BLACK


def render_graph(title, unit, values, warn):
    img, d = _canvas()
    small, big = _font(16), _font(48)
    values = list(values)[-HISTORY:]
    latest = values[-1] if values else None

    d.text((8, 8), title.replace("₂", "2") + " >", font=small, fill=BLACK)  # no subscripts in Pirkkala
    d.line([(0, TOP_H - 1), (WIDTH, TOP_H - 1)], fill=GREY)
    if latest is None:
        d.text((8, 52), "no data", font=small, fill=GREY)
    else:
        text = f"{latest:.0f}" if abs(latest) >= 100 or float(latest).is_integer() else f"{latest:.1f}"
        d.text((8, 36), text, font=big, fill=_value_color(latest, warn))
        d.text((14 + d.textlength(text, font=big), 66), unit, font=small, fill=GREY)

    # The y range covers the data and the warn level, so the warn line always shows.
    nums = [v for v in values if v is not None]
    lo, hi = min(nums + [warn]) if nums else 0, max(nums + [warn])
    if hi == lo:
        lo, hi = lo - 1, hi + 1
    pad = (hi - lo) * 0.05
    lo, hi = max(0, lo - pad) if lo >= 0 else lo - pad, hi + pad

    def xy(i, v):  # right-aligned: the newest sample is at "now"
        return (GX0 + (HISTORY - len(values) + i) * (GX1 - GX0) / (HISTORY - 1),
                GY1 - (v - lo) / (hi - lo) * (GY1 - GY0))

    d.line([(GX0, GY0), (GX0, GY1), (GX1, GY1)], fill=GREY)
    fmt = ".0f" if hi - lo >= 10 else ".1f"
    for label, y in ((format(hi, fmt), GY0), (format(lo, fmt), GY1)):
        d.text((GX0 - 4 - d.textlength(label, font=small), y - 8), label, font=small, fill=GREY)
    d.text((GX0, GY1 + 6), "-10 min", font=small, fill=GREY)
    d.text((GX1 - d.textlength("now", font=small), GY1 + 6), "now", font=small, fill=GREY)

    yw = xy(0, warn)[1]
    for x in range(GX0 + 2, GX1, 8):
        d.line([(x, yw), (min(x + 4, GX1), yw)], fill=ORANGE)

    run = []  # consecutive samples form one line; None breaks it
    for i, v in enumerate(values + [None]):
        if v is not None:
            run.append(xy(i, v))
        elif run:
            d.line(run, fill=BLACK) if len(run) > 1 else d.point(run, fill=BLACK)
            run = []
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
