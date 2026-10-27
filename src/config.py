#config.py — tunable values for enkakumon. No logic in this file.

# --- Sampling ---
SAMPLE_HZ = 1

# --- SCD40 ---
TEMP_OFFSET_C = 0.0        # subtracted from SCD40 temperature

# --- Microphone ---
MIC_NAME = "googlevoicehat"  # substring of the ALSA device name
MIC_RATE = 48000
MIC_BLOCK = 4800           # samples per callback (0.1 s at 48 kHz)
DB_OFFSET = 120.0

# --- Display pins (used in Step 2) ---
DC_PIN = 25
RST_PIN = 24

# --- SGP40 fallback compensation (used before SCD40 has data) ---
DEFAULT_TEMP_C = 25.0
DEFAULT_RH = 50.0

