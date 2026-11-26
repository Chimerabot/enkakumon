# REFLECTION — Navin Mani

CS370 term project (enkakumon). One page (handout §12). Write it in your
own words near the end of the project; keep notes here as things happen.

## Where the agent was most reliable against real hardware

<!-- Examples to consider: datasheet conversions and CRC vectors that
     passed unit tests; parts of the daemon that worked on the Pi first try. -->

## Where the agent was least reliable against real hardware

<!-- Example to consider: the LSM6DSOX pedometer registers copied from the
     Adafruit driver (fixed by Gideon in 9d1f29e). -->

## A bug the agent introduced that I caught, and how

1. **The test (`test_pet.c:438–451`) with the pet asleep:**

* This test sends one reading with `motion = 1.5`, which is above the threshold of `1.0` so the pet wakes up.
* After the pet wakes up, the code sends 58 seconds of bad air (`CO₂ = 1600`) and checks to see if the health is still 80. If it is at 80, that means the full minute cycle hasn't passed yet.
* After that, the code sends 4 seconds of bad air (62 in total) and checks to see if the health is 78. One full minute of bad CO₂ should cost −2, but it added an extra counting for the waking call, so health came out to 79.

2. **Why it was 79 and not 78:**

* The code woke the pet and reset the minute once when the pet woke up, and then kept going in the call so the waking reading was added to the minute's samples.
* That reading had `motion` set equal to `1.5`, and the other 61 readings had no motion value (`NAN`), so they weren't counted.
* After a minute, the motion average was `1.5 ÷ 1 = 1.5`, which is above the activity threshold of `0.5`, so the pet got +1 for activity.
* `−2 + 1 = −1`, so health was 79.

3. **Where the agent's mistake was:**

* The comment above that test said that bad air present will cost the pet's health to go down after a minute "whether the waking call itself counts or not".
* The comment was misleading: the waking call does matter. When my code counted the waking call's motion, the pet earned the +1, which caused the test to fail. The check only passes if the waking call is not counted.

4. **The fix:**

* **Test:** For the fix, the agent rewrote the comment to say that the waking call is not a sample—its motion must not count as activity—and the check itself (`== 78`) stays the same.
* **My code:** In the wake branch, I added `update_mood(p); return;` after `reset_minute(p);`. This ends the waking call right at `reset_minute(p)` so nothing from it is counted.

5. **The reasoning behind the rule:**

* Motion that wakes the pet happens when the user picks it up or shakes it. This triggers the wake-up signal, not exercise. If it also counted as activity, every wake-up would give a +1.

- What I learned from this: The bug provided showed me what would happen if the pet's device was adding readings that weren't supposed to added after a certain point has reached. This bug, showed me what the pet was doing and how it added an additional read instead of stopping after the device has collected the data, and the fix showed me what would have happened if the extra count was added. It would have provided an extra reading that wasn't necessary and it would have messed up the data collected for that certain reading once the pet device collected it.

## Notes as we go
- "Nov 17- 18: wrote all Step 3 rules; tests green."

