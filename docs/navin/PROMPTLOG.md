# PROMPTLOG — Navin Mani

CS370 term project (enkakumon). Per the handout (§12): 6–10 annotated
episodes from my Claude Code sessions. Required among them:

- [ ] one **revised plan**
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

<!-- Write each episode in your own words. The candidates below are
     reminders of what happened, with pointers; pick 6–10 and annotate
     them. Delete this comment and the candidate list when done. -->

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
8. **Pending — Pi hardware evidence.** Step 2/3 acceptance runs and touch
   calibration on the Pi. *Needed for the hardware debugging-loop episode.*
