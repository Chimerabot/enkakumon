"""
sensors.py — read every enkakumon sensor; failing devices report None.

Acceptance tests:
  python sensors.py prints values every second for 2 minutes without crashing. Breathing on the board raises co2; covering the APDS9960 raises proximity; shaking raises motion; clapping raises db.
"""

import math
import time
import threading
from collections import deque

import board
import numpy as np
import sounddevice as sd

import adafruit_scd4x
import adafruit_sgp40
from adafruit_apds9960.apds9960 import APDS9960
from adafruit_lsm6ds.lsm6dsox import LSM6DSOX

import config


class Sensors:
    def __init__(self):
        self._logged = set()   # device names whose error was already printed

        # Cached SCD40 values (it only has new data every 5 s)
        self._co2 = None
        self._temp_c = None
        self._rh = None

        # Mic state, written by the audio callback
        self._mic_blocks = deque(maxlen=10)   # recent block mean-squares, ~1 s
        self._stream = None

        try:
            i2c = board.I2C()
        except Exception as e:
            i2c = None
            self._log("i2c", e)

        # Each device gets its own try/except so one failure doesn't stop the rest.
        try:
            self.scd = adafruit_scd4x.SCD4X(i2c)
            self.scd.start_periodic_measurement()
            
        except Exception as e:
            self.scd = None
            self._log("scd40", e)

        # TODO: same pattern for self.sgp (SGP40)
        try:
            self.sgp = adafruit_sgp40.SGP40(i2c)
        except Exception as e:
            self.sgp = None
            self._log("sgp40", e)
            
        self._voc = None
        if self.sgp is not None:
            threading.Thread(target=self._voc_loop, daemon=True).start()



        # TODO: same pattern for self.apds (APDS9960)
        try:
            self.apds = APDS9960(i2c)
            self.apds.enable_proximity = True
            self.apds.enable_color = True
        except Exception as e:
            self.apds = None
            self._log("apds9960", e)

        # TODO: same pattern for self.imu (LSM6DSOX)
        try:
            self.imu = LSM6DSOX(i2c)
            self.imu.pedometer_enable = True
        except Exception as e:
            self.imu = None
            self._log("lsm6dsox", e)

        # TODO: same pattern for the mic: call self._start_mic() inside try
        try:
            self._start_mic()
        except Exception as e:
            self._stream = None
            self._log("mic", e)

    # ---------- helpers ----------

    def _log(self, name, err):
        """Print an error only the first time it happens for a device."""
        if name not in self._logged:
            print(f"Sensor error [{name}]: {err}")
            self._logged.add(name)

    def _start_mic(self):
        device = None
        for i, dev in enumerate(sd.query_devices()):
            if config.MIC_NAME in dev["name"] and dev["max_input_channels"] > 0:
                device = i
                break
        if device is None:
            raise RuntimeError(f"Microphone matching '{config.MIC_NAME}' not found.")
            
        self._stream = sd.InputStream(
            device=device,
            samplerate=config.MIC_RATE,
            channels=1,
            dtype="int32",
            blocksize=config.MIC_BLOCK,
            callback=self._audio_callback,
        )
        self._stream.start()

    def _audio_callback(self, indata, frames, time_info, status):
        # Runs on the audio thread. Keep it fast: no prints, no I2C.
        x = indata[:, 0].astype(np.float64)
        x -= np.mean(x)
        self._mic_blocks.append(np.mean(x**2))

    # ---------- per-device reads ----------

    def _voc_loop(self):
        # The VOC index algorithm assumes exactly one sample per second.
        next_t = time.monotonic()
        while True:
            self._voc = self._read_voc()
            next_t += 1.0
            time.sleep(max(0, next_t - time.monotonic()))
    
    def _read_scd(self):
        if self.scd is None:
            return
        try:
            if self.scd.data_ready:
                self._co2 = self.scd.CO2
                self._temp_c = self.scd.temperature - config.TEMP_OFFSET_C
                self._rh = self.scd.relative_humidity 
        except Exception as e:
            self._log("scd40", e)
            self._co2 = None
            self._temp_c = None
            self._rh = None

    def _read_voc(self):
        #return None if self.sgp is None
        if self.sgp is None:
            return None
        # pick temp/rh: cached SCD40 values, or config defaults if None
        temp = self._temp_c
        if temp is None:
            temp = config.DEFAULT_TEMP_C
        
        rh = self._rh
        if rh is None:
            rh = config.DEFAULT_RH
            
        # TODO: return self.sgp.measure_index(temperature=..., relative_humidity=...)
        # inside try/except -> log + return None on error
        try:
            return self.sgp.measure_index(temperature = temp, relative_humidity = rh)
            
        except Exception as e:
            self._log("sgp40", e)
            return None
   

    def _read_apds(self):
        # TODO: return (light, proximity), or (None, None) on failure
        #       hint: color_data is (r, g, b, c); light is c
        if self.apds is None:
            return None, None
        try:
            _, _, _, light = self.apds.color_data
            proximity = self.apds.proximity
            return light, proximity
        except Exception as e:
            self._log("apds9960", e)   
            return None, None 

    def _read_imu(self):
        # TODO: return (motion, steps), or (None, None) on failure
        #       motion = abs(sqrt(ax^2 + ay^2 + az^2) - 9.81)
        if self.imu is None:
            return None, None
        try:
            ax, ay, az = self.imu.acceleration
            motion = abs (math.sqrt(ax**2 + ay**2 + az**2)- 9.81)
            steps = self.imu.pedometer_steps
            return motion, steps
        except Exception as e:
            self._log("lsm6dsox", e)
            return None, None
   

    def _read_db(self):
        # TODO: return None if no stream or no blocks yet
        if self._stream is None or not self._stream.active or not self._mic_blocks:
            return None
        try:
            # TODO: mean_sq = average of self._mic_blocks; rms = sqrt(mean_sq)
            mean_sq = sum(self._mic_blocks) / len(self._mic_blocks)
            rms = math.sqrt(mean_sq)

        # TODO: guard rms == 0 (log10(0) is -inf)
            if rms == 0:
                return None

        # TODO: return 20 * log10(rms / 2**31) + config.DB_OFFSET
            return 20 * math.log10(rms / (2**31)) + config.DB_OFFSET

        except Exception as e:
            self._log("mic", e)
            return None

    # ---------- public ----------

    def read(self):
        """Return all readings as a dict. Must take < 100 ms; no sleeps here."""
        self._read_scd()
        voc = self._voc
        light, proximity = self._read_apds()
        motion, steps = self._read_imu()
        db = self._read_db()

        return {
            "co2": self._co2,
            "temp_c": self._temp_c,
            "rh": self._rh,
            "voc": voc,
            "light": light,
            "proximity": proximity,
            "motion": motion,
            "steps": steps,
            "db": db,
        }


def _fmt(v):
    # TODO: return "--" for None, a rounded string for floats, str() otherwise
    if v is None:
        return "--"
    if isinstance(v, float):
        return f"{v:.1f}"
    return str(v)

if __name__ == "__main__":
    s = Sensors()
    period = 1.0 / config.SAMPLE_HZ
    next_t = time.monotonic()
    while True:
        r = s.read()
        # TODO: print one line like "co2=612 temp_c=22.4 ... db=41.3" using _fmt
        line = " ".join(f"{k}={_fmt(v)}" for k, v in r.items())
        print(line)
        next_t += period
        # TODO: sleep for max(0, next_t - time.monotonic())
        time.sleep(max(0, next_t - time.monotonic()))


