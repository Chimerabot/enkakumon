# enkakumon V1 UX Guidelines

## Context and rules

You are implementing UX for the enkakumon digital pet project. Currently extant are all features from `enkakumon v1 code plan.md`. The primary goal is gamifying the basic features and setup already completed, into a digital pet which is affected in various aspects by data read from the various sensors connected to the device.

Architecture is the same as described in the above-mentioned file.

All additions must be made under the following assumptions:
- The Pi, as soon as it is turned on, will run the full digital pet system: sensor daemon, data storage, and gamified features. It should not require initialization from another device. The systemd units `enkakumon.service` (daemon) and `enkakumon-ui.service` (UI) start both at boot once they are installed with `make && sudo make install`.
- The Pi will be running on battery power in its finished form. Its battery life, while running all sensors, the screen, and the digital pet system, should last at minimum 48 hours. Keep code essentially minimal and light, especially that which is running constantly.
- The Pi should always store data from the past 48 hours, so it can be accessed and used when necessary.

## UI Details

The UI for the enkakumon system will have two main sides: Pet and Data. Pet contains all the tamagotchi-like digital pet game elements; Data contains graphs showing sensor data from the past 48 hours, one metric per screen as it is now.

Two buttons at the bottom of the touchscreen ("Pet" and "Data") will allow users to switch between these modes at will. The current side's button is filled black. The Pet side shows when the UI starts.

The color scheme should be black-on-white for both the pet and the data graphs, with colors where necessary to convey important information. Otherwise, monochrome should be preferred.

Use the `assets/pirkkala.ttf` font for text. Use text minimally. Pirkkala is a pixel font: render it only at 16, 32 or 48 px, where it stays crisp. It covers ASCII only, with no subscript or degree glyphs, so on-screen text must be ASCII (`display.py` already renders "CO₂" as "CO2").

## UX Architecture

**Pet:**
- Visuals: The pet's state will be conveyed via the pixel art sprites in `assets/enkakumon-sprite.png`. Each frame is 100x100 pixels. The first is the normal (idle) state; the second is the sleeping state; the third is the unhealthy state; and the last is the dead state. More may be added eventually, but for now, these static frames will visually convey the pet's state. The frame is drawn at 2x, black on white as drawn. Below it, a health bar shows `health`: black, or red at 50 and below.
- Health: The pet's health will be managed by an overarching `int` variable `health`, which ranges from 0-100 as a percentage health value. It defaults to `100` when the program first starts. The unhealthy state begins at `health = 50`, and dead at `0`. 
- Health calculations: `health` should update once every five minutes, adding or subtracting `1` based on whether environmentally significant variables have improved or declined during that period. The significant variables and their deciding values (the `PET_*_MAX` keys in `enkakumon.conf`) are:

  | Variable | Sensor | Bad side |
  | --- | --- | --- |
  | CO2 | SCD40 | above 1000 ppm |
  | Air quality (VOC index) | SGP40 | above 150 |
  | Noise | microphone | above 70 dB SPL |
  | Proximity | APDS9960 | above 200 of 255 (the pet is covered) |

  Each variable is averaged over the five minutes, skipping missing readings. If any average is on the bad side, `health -= 1`; otherwise `health += 1`. If no variable had readings, health does not change. Health stays within 0-100. The first three values are also the Data graphs' warn lines.
- Sleep: the pet sleeps when it is dark (APDS9960 clear channel below 10, `PET_DARK_LIGHT`) but not covered, for 30 seconds. It wakes as soon as it is light. Health updates and surprises pause while it sleeps, and a surprise in progress is cancelled without a penalty. Being covered is not sleep: it counts as bad proximity.
- Death: at `health = 0` the pet is dead. Health and surprises stop, and the screen reads "tap to hatch". A tap on the pet hatches a new one at `100`.
- Surprises: On occasion, the pet should make use of its non-environmentally significant sensors to give the user surprise tasks which change its health by more significant amounts. A prompt at the top of the Pet screen names the task.
     - Color sensor: The pet asks to see red, green or blue (the prompt shows a swatch of the color). It is fulfilled when the APDS9960's channel for that color reads at least 1.5 times each of the other two (`PET_COLOR_RATIO`). If the request is fulfilled within 30 seconds, `health += 15`; if not `health -= 15`.
     - Pedometer: The pet asks to go on a walk. If the pedometer counts 10 new steps (`PET_WALK_STEPS`) within 30 seconds, `health += 15`; if not `health -= 15`.
     - Microphone: The pet asks to hear music. If the microphone reads at least 60 dB SPL (`PET_MUSIC_DB`) for 5 seconds in total (`PET_MUSIC_S`) within 30 seconds, `health += 15`; if not `health -= 15`.
     - These surprises come at random intervals of 10 to 20 minutes of awake time, one at a time, with the three kinds equally likely.
- Data: `health` is saved to `pet.state` every 60 seconds and when the daemon stops, so a shutdown freezes the pet (a dead pet stays dead) rather than resetting it.

**Data:**
- Visuals: line graphs drawn with Pillow by `render_graph` in `src/ui/display.py`, as currently implemented. Each screen shows one metric (CO2 in ppm, air quality as the VOC index, noise in dB SPL). From top to bottom it has the title, the current value in large text with its unit, and a line graph with min/max labels on the y axis and time labels on the x axis. Text is black at 16 px for the title and labels and 48 px for the value; secondary text (unit, axis labels, status messages) is grey. Missing samples break the line, and when the latest value is missing "no data" replaces the value.
- Color: color is used only for warn levels. A dashed orange line marks the metric's warn level (CO2 1000 ppm, VOC 150, noise 70 dB). The current value turns orange above the warn level and red above 1.5× it.
- Navigation: the metric screens are cycled using a dedicated button at the top of the screen: the metric's name, marked with ">".
- Time (not yet built; needs the 48-hour history store planned in DESIGN.md): Another button at the top of the screen cycles through time periods. Until then the graphs cover the last 10 minutes. The maximum should be the full 48-hour data saving period, and the minimum should be the past hour. If the device detects that there has been a skip in time (i.e. the device has been powered off for some period of time), a graphical line should be simply rendered on the graph to indicate that there has been a time skip (assuming this is possible).