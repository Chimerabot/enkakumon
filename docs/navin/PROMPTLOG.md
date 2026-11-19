# PROMPTLOG — Navin Mani

CS370 term project (enkakumon). Per the handout (§12): 6–10 annotated
episodes from my Claude Code sessions. Required among them:

- [x] one **revised plan**
- [ ] one **rejected diff**
- [ ] one **tool-output debugging loop with real hardware evidence** (paste the evidence)
- [ ] one **review of my partner's work** (what I questioned, the answer, what changed)

Transcripts (raw `.jsonl`, copied out at each milestone):
- `14a82e35-935d-4d76-943a-69db00b536f1.jsonl` — Oct 26 (project start, `sensors.py`)
- `34f4ad29-2829-47c6-8528-98b08c58aff2.jsonl` — Oct 26 → (Steps 1–3)

## Episode template

```
### Episode N: <short title>
- Date / session: <date>, <transcript file>
- Commits: <hashes>
- What I asked:
- What the agent proposed or did:
- My judgment (what I questioned, accepted, rejected or changed, and why):
- Evidence (test output, dmesg, Pi output, screenshots):
- Outcome / what I'd do differently:
```

## Episodes

### Episode 1: Step 3 spec conflict

- Date / session: Nov 16, 2026, 34f4ad29-2829-47c6-8528-98b08c58aff2.jsonl

- Commits: 87be764, d547488, a5444b9, 3d95df4, 80813b8 (all on branch m3-pet-health)

- Gideon's changes since Step 2:
- Fixed the pedometer (9d1f29e);
- Gideon also changed the UI: "warming up" text, outages drawn as gaps, memcheck replacing asan on the Pi (4b5d479);
- Merged Step 2 (PR #2) and added DESIGN.md, a UX doc, a sprite and fonts;
and also removed the commit log from CLAUDE.md.

Three versions of the health system that disagreed:
- First: Code plan Step 3: start at 80, summed rules every minute, moods happy/ok/sick/sleeping, sleep when covered or dark.

- enkakumon ux.md: will start at 100, and updates every 5 minutes, while unhealthy at 50, dead at 0, sprite frames, "surprise" tasks  are worth ±15. Some thresholds were still blank ("[deciding values]").

- The code plan's status note: the health system "is specified in a separate plan", which wasn't in the repo.

Two more observations:
- DESIGN.md §7 assigns "Code implementation" for me to write out, and handout §3.3 says the product's intelligence must be code the students wrote.

What I decided: the agent asked I two questions.

- Which spec? I chose the code plan's Step 3.
- Who writes the rules? I told the agent that "I will write the rules": "I will  write pet_update/pet_tap/pet_worst; the agent does save/load, plumbing, the Pet screen, and the tests first".

What I approved:

The agent's suggested rule details:
- minute averages, so one spike doesn't cost health;
- "clean air" means CO₂ and VOC only;
- a placeholder for dark threshold is set up as 10;
- wake on a tap or motion above 1.0;
- a step count that goes backwards counts as 0;
- the worst texts, with ties going CO₂ → VOC → noise → temp → RH.

- Using Gideon's sprite instead of an AI-drawn face, because of DESIGN.md §8.
- Two small additions: ~/ support in PET_PATH, and the PET_SAVE_S setting.

Revisions during the work:
- While re-reading its own tests, the agent found two problems and fixed them before handing them over:
- a test that assumed one particular way of counting the call that wakes the pet;
- an unstated rule for how dt counts toward the 30 s and the minute, now written into pet.h (80813b8).

- Evidence: the rule tests show that 153 checks, 51 failures remain the same until I write code exists have been done. The agent checked that they're passable with a throwaway version, which it then deleted.

- My judgment: I choose the code plan in order to give me an idea of what will now be added into the project files since step 3 was just implemented after 2 weeks and updates were added to the code from Gideon. This now gave me a idea of what the code plan was in terms of adding things like the pet's mode based off things like temperature, ari quaility, and the level of CO2 levels, just to name a few. This gave me an idea of what the digtial pet could showcase when the device is in use. I chose to write the rules myself because it gave me a better idea of what the pet decive is checking for in terms of CO2 levels, temperature, and air levels, just to name a few. This gives me a step by step of what the device is checking and what needs to be fixed or tweeked up if fixes are needed.

### Episode 2: The invisible sprite
- Date / session: Nov 16, 2026, same transcript
- Commit: 3d95df4

- What the agent did: it wrote render_pet:

- the sprite frame for each mood now showcases the different states of what the device will showcase when in use. Ex: (idle for happy/ok, unhealthy for sick, sleeping for sleeping), scaled 2×;
- the mood name;
- a health bar;
- the worst line.

The problem:
- It rendered 6 test frames to a PNG and looked at them.The text, health bars and colors were right, but the sprite was missing on every frame.
- No error was logged, so the file had loaded.

The investigation:
- A pixel count showed every visible pixel in all four frames was pure black (0,0,0): 1280, 1178, 1371 and 2733 pixels.
- Viewed on a grey background, the sprite turned out to be black line art on a transparent background. The UI draws on black, so the art was invisible.

The fix:
- The sprite's transparency is used as a stencil and filled with one of the four mood colors: green indicates happy, yellow indicates ok, red indicates sick, blue indicates sleeping.
- Gideon's drawing itself is unchanged, which keeps to DESIGN.md §8 (no AI-made art). It also matches the code plan's "face colored by mood".
- Tested end to end: a saved pet.state with health 55 went through the daemon and the JSON, and appeared on the Pet screen, which showed first. Taps cycled Pet → CO₂ → Air quality → Noise → Pet.

- Options I considered: a white background for just the Pet screen, inverting the image, asking Gideon for a light-on-dark version of the sprite, or drawing a face with Pillow (ruled out by §8).
 
- My judgment: Tinting the sprite for now gave me an idea of how the sprite will look but it also gave me an idea of how the sprite will look when the colors for the different modes are present and shown whenever the device runs. Tinting the sprite gave me an outline of the sprite and once the color modes are shown, which will be, in the demo video it shows me whether tinting the sprite caused the colors to mix with the tinting or if the colors of the modes do not appear at all.

### Episode 3: Writing the health rules
Date / session: Nov 18, 2026, 

- What I asked: for review only. I wrote pet_update, pet_tap and pet_worst myself against the tests the agent wrote first.
- What the agent did: reviewed each version and pointed to line numbers. It didn't write rule code.
- Progress: failures went 45 → 39 → 23 → 20 → 13 → 12 → 0 as I added mood, CO₂, the other rules, steps, sleep/wake and pet_worst.
- Bugs caught in review:
- VOC average divided by co2_count, which is a divide-by-zero when there's no CO₂ reading.
- An extra } closed the minute block early.
- step_sum was never reset.
- The reset_minute call was missing after I moved the resets into the helper.
- Line 450: the waking motion counted as activity, so health was 79 instead of 78. The test comment said "whether the waking call counts or not", which was wrong. The agent corrected the comment and kept the check, and I made the waking call return early.
- Evidence: 153 checks, 0 failures; make asan and make memcheck both exit 0.

- My judgment: I choose to write the rules myself and asked only for review, because it gave me an idea of what the pet needed to keep in track of while also seeing how each rule, like C02 levels, air quaility, temperature, and noise level, just to name a few, are going to be calcuted and how the code for each rule was running the pet based on changes and  patterns that can occur when the pet is put into use.
- The bug taught me what can happen if an overload of data occurs within the pet. If the extra cycle/run happened, then the data and cycles that the pet was able to collect before can put a road block for giving the user the best possbile read of data for what the pet was able to collect. Adding extra cycles who have caused future errors where the pet might read in data that's false and wasn't needed for the current readings it was trying to collect.
- Yes, I agree with keeping == 78. A single shake to wake the pet isn't exercise, and if it's counted alongside 59 NAN readings, that one jolt skews the whole minute's average and falsely triggers an activity bonus. Ending the wake-up call early keeps waking and sleeping symmetrical—and ensures that the bug was fixed rather than weakening the test to match broken behavior

Outcome / what I'd do differently:

- All sleep/wake and pet_worst tests now pass. Next time, I'd test incrementally after each state transition rather than implementing multiple branches at once, and double-check pointer member access (p->conf->...) and boundary returns as I write them.


### Candidates (facts only — not yet annotated)

1. **Oct 27 — Step 1 plan, pedometer registers.** The agent planned first
   (CLAUDE.md rule) and flagged that the Adafruit driver's pedometer uses
   LSM6DS33 registers, offering datasheet-correct DSOX registers instead.
   I chose to mirror the driver, as the code plan said. Gideon later
   replaced it with the embedded-bank registers (`9d1f29e` "Fix pedometer").
   *Possible fit: revised plan; reflection "least reliable against hardware".*
2. **Oct 27 — Lab build failure.** `make` failed with
   `bits/c++config.h: No such file`; cause was `C_INCLUDE_PATH` pointing at
   gcc's C++ headers in my shell profile. Fixed by unsetting it, no code
   change. *Tool-output debugging loop (not hardware).*
3. **Oct 27 — Stale-socket guard seen in practice.** A botched test command
   left a daemon running; the second instance refused the live socket and
   exited 2 instead of stealing it.
4. **Oct 28 — Step 2 rendered frames.** Frames rendered to PNG showed the
   newest data was not at "now", axis labels were inconsistent and the empty
   graph had a negative axis; fixed before committing (`decc1d1`).
   *Possible fit: revised plan / rejected diff.*
5. **Oct 28 — GitHub push 403.** Push denied to `manina57`; a fine-grained
   token cannot push to a personal-account repo as a collaborator. A classic
   token with `repo` scope worked.
6. **Oct 28 / Nov 15 — Gideon's changes to my code.** `9d1f29e` (pedometer
   registers) and `4b5d479` (UI warm-up text, outage gaps, quiet refusal;
   memcheck replaces asan on the Pi). *Candidate for the partner review.*
7. **Nov 16 — Step 3 spec conflict.** The code plan's Step 3, the UX doc and
   the plan's status note disagreed on the health system. Chose the code plan;
   split the work so I write the rules in `pet.c` (handout §3.3).
   *Possible fit: revised plan.*
