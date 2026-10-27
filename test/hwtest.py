#!/usr/bin/env python3
import time
import numpy as np

results = {}

def check(name, fn):
    try:
        detail = fn()
        results[name] = ("PASS", detail)
    except Exception as e:
        results[name] = ("FAIL", f"{type(e).__name__}: {e}")
    status, detail = results[name]
    print(f"[{status}] {name}: {detail}")

# ---------- I2C ----------
import board
i2c = board.I2C()

def i2c_scan():
    while not i2c.try_lock():
        pass
    try:
        found = [hex(a) for a in i2c.scan()]
    finally:
        i2c.unlock()
    expected = {"0x39": "APDS9960", "0x48": "TSC2007", "0x59": "SGP40",
                "0x62": "SCD40", "0x6a": "LSM6DSOX"}
    missing = [f"{n} ({a})" for a, n in expected.items() if a not in found]
    if missing:
        raise RuntimeError(f"found {found}, missing {missing}")
    return f"found {found}"

def scd40():
    import adafruit_scd4x
    s = adafruit_scd4x.SCD4X(i2c)
    s.start_periodic_measurement()
    try:
        for _ in range(70):  # first reading takes ~5 s
            if s.data_ready:
                return f"CO2 {s.CO2} ppm, {s.temperature:.1f} C, {s.relative_humidity:.0f}% RH"
            time.sleep(0.1)
        raise TimeoutError("no data after 7 s")
    finally:
        s.stop_periodic_measurement()

def sgp40():
    import adafruit_sgp40
    return f"raw gas {adafruit_sgp40.SGP40(i2c).raw}"

def apds9960():
    from adafruit_apds9960.apds9960 import APDS9960
    a = APDS9960(i2c)
    a.enable_proximity = True
    a.enable_color = True
    time.sleep(0.3)
    r, g, b, c = a.color_data
    return f"proximity {a.proximity}, light (clear) {c}"

def lsm6dsox():
    from adafruit_lsm6ds.lsm6dsox import LSM6DSOX
    m = LSM6DSOX(i2c)
    ax, ay, az = m.acceleration
    gx, gy, gz = m.gyro
    return f"accel ({ax:.1f}, {ay:.1f}, {az:.1f}) m/s2, gyro ({gx:.2f}, {gy:.2f}, {gz:.2f}) rad/s"

# ---------- Audio ----------
import sounddevice as sd

def find_device(keyword, kind):
    for i, d in enumerate(sd.query_devices()):
        if keyword in d["name"].lower() and d[f"max_{kind}_channels"] > 0:
            return i
    raise RuntimeError(f"no {kind} device matching '{keyword}'")

def microphone():
    dev = find_device("voicehat", "input")
    channels = min(2, sd.query_devices(dev)["max_input_channels"])
    rec = sd.rec(48000, samplerate=48000, channels=channels, dtype="int32", device=dev)
    sd.wait()
    x = rec[:, 0].astype(np.float64)
    x -= x.mean()
    rms = np.sqrt(np.mean(x ** 2))
    if rms == 0:
        raise RuntimeError("all-zero samples (check DOUT/LRCL/SEL wiring)")
    db = 20 * np.log10(rms / 2 ** 31) + 120
    return f"~{db:.0f} dB SPL (clap and rerun to see it rise)"

def speaker():
    try:
        dev = find_device("headphones", "output")
    except RuntimeError:
        dev = None  # fall back to default output
    t = np.linspace(0, 1, 48000, endpoint=False)
    tone = (0.3 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    sd.play(tone, samplerate=48000, device=dev)
    sd.wait()
    if input("  Did you hear a 1-second tone? [y/n] ").strip().lower() != "y":
        raise RuntimeError("no tone heard (check trimmer, pin 32, audio overlay)")
    return "tone heard"

# ---------- Display + touch ----------
def display_and_touch():
    import digitalio
    from PIL import Image, ImageDraw, ImageFont
    from adafruit_rgb_display import ili9341
    import adafruit_tsc2007

    disp = ili9341.ILI9341(
        board.SPI(),
        cs=digitalio.DigitalInOut(board.CE0),
        dc=digitalio.DigitalInOut(board.D25),
        rst=digitalio.DigitalInOut(board.D24),
        baudrate=24000000,
    )
    w, h = disp.width, disp.height
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 22)
    except OSError:
        font = ImageFont.load_default()

    img = Image.new("RGB", (w, h), "black")
    draw = ImageDraw.Draw(img)
    for i, color in enumerate(["red", "green", "blue", "yellow", "cyan", "magenta"]):
        draw.rectangle((i * w // 6, 0, (i + 1) * w // 6, h // 3), fill=color)
    draw.text((20, h // 2), "Tap the screen", font=font, fill="white")
    disp.image(img)

    if input("  Do you see color bars and 'Tap the screen'? [y/n] ").strip().lower() != "y":
        raise RuntimeError("display not showing image (check IM1-3 jumpers, CS/DC/RST)")

    ts = adafruit_tsc2007.TSC2007(i2c)
    print("  Tap the screen within 15 s...")
    end = time.time() + 15
    while time.time() < end:
        if ts.touched:
            p = ts.touch
            x = int(p["x"] / 4095 * (w - 1))
            y = int(p["y"] / 4095 * (h - 1))
            draw.rectangle((0, h // 3, w, h), fill="black")
            draw.text((20, h // 2), "Touch OK!", font=font, fill="lime")
            draw.ellipse((x - 10, y - 10, x + 10, y + 10), outline="white", width=3)
            disp.image(img)
            return f"display OK; touch raw x={p['x']} y={p['y']} pressure={p['pressure']}"
        time.sleep(0.02)
    raise TimeoutError("display OK, but no touch detected (check X+/X-/Y+/Y- wires)")

# ---------- Run ----------
print("[ enkakumon hardware check ]")
check("I2C scan", i2c_scan)
check("SCD40 CO2", scd40)
check("SGP40 VOC", sgp40)
check("APDS9960 light/proximity", apds9960)
check("LSM6DSOX motion", lsm6dsox)
check("Microphone", microphone)
check("Speaker", speaker)
check("Display + touch", display_and_touch)

passed = sum(1 for s, _ in results.values() if s == "PASS")
print(f"\n=== {passed}/{len(results)} passed ===")
for name, (status, _) in results.items():
    if status == "FAIL":
        print(f"  FAIL: {name}")
