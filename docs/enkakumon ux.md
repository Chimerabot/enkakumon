# enkakumon V1 UX Guidelines

## Context and rules

You are implementing UX for the enkakumon digital pet project. Currently extant are all features from `enkakumon v1 code plan.md`. The primary goal is gamifying the basic features and setup already completed, into a digital pet which is affected in various aspects by data read from the various sensors connected to the device.

Architecture is the same as described in the above-mentioned file.

All additions must be made under the following assumptions:
- The Pi, as soon as it is turned on, will run the full digital pet system: sensor daemon, data storage, and gamified features. It should not require initialization from another device, as it currently does for testing purposes.
- The Pi will be running on battery power in its finished form. Its battery life, while running all sensors, the screen, and the digital pet system, should last at minimum 48 hours. Keep code essentially minimal and light, especially that which is running constantly.
- The Pi should always store data from the past 48 hours, so it can be accessed and used when necessary.

## UI Details

The UI for the enkakumon system will have two main sides: Pet and Data. Pet contains all the tamagotchi-like digital pet game elements; Data contains graphs showing sensor data from the past 48 hours, scrollable in the same way it is now.

Two buttons at the bottom of the touchscreen ("Pet" and "Data") will allow users to switch between these modes at will.

The color scheme should be black-on-white for both the pet and the data graphs, with colors where necessary to convey important information. Otherwise, monochrome should be preferred.

Use the `pirkkala.ttf` font for text. Use text minimally.

## UX Architecture

**Pet:**
- Visuals: The pet's state will be conveyed via the pixel art sprites in `assets/enkakumon-sprite.png`. Each frame is 100x100 pixels. The first is the normal (idle) state; the second is the sleeping state; the third is the unhealthy state; and the last is the dead state. More may be added eventually, but for now, these static frames will visually convey the pet's state.
- Health: The pet's health will be managed by an overarching `int` variable `health`, which ranges from 1-100 as a percentage health value. It defaults to `100` when the program first starts. The unhealthy state begins at `health = 50`, and dead at `0`. 
- Health calculations: `health` should update once every five minutes, adding or subtracting `1` based on whether environmentally significant variables ([air quality variables, noise levels, proximity]) have improved or declined during that period. This should be decided based on whether these significant variables are on either side of [deciding values for variables].
- Surprises: On occasion, the pet should make use of its non-environmentally significant sensors to give the user surprise tasks which change its health by more significant amounts.
     - Color sensor: The pet asks to see a certain color. If the request is fulfilled within 30 seconds, `health += 15`; if not `health -= 15`.
     - Pedometer: The pet asks to go on a walk. If the pedometer starts getting [enough input] within 30 seconds, `health += 15`; if not `health -= 15`.
     - Microphone: The pet asks to hear music. If the microphone level is high enough within 30 seconds, `health += 15`; if not `health -= 15`.
     - [other ideas]
     - These surprises should come at varying periods while the device is powered on, usually every 10+ minutes at random intervals.
- Data: `health` should be periodically saved to memory so that the pet's data is not lost when shutting down the Pi (a shutdown should simply freeze its stats and already-read data, not reset it or delete it).

**Data:**
- Visuals: Panda graphs, as are currently implemented. They can be cycled using a dedicated button at the top of the screen.
- Time: Another button at the top of the screen cycles through time periods. The maximum should be the full 48-hour data saving period, and the minimum should be the past hour. If the device detects that there has been a skip in time (i.e. the device has been powered off for some period of time), a graphical line should be simply rendered on the graph to indicate that there has been a time skip (assuming this is possible).