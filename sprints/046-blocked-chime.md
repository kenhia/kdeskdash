# Sprint 046 — Audible chime when a Claude session goes BLOCKED

**Proposal:** korg:4425 (a slice of program korg:4368, "Backlog drawdown, November 2026").
**Covers:** WI 1096. Ken's answer of 2026-11-09 was to build it for rpidash3.
**Branch:** `046-blocked-chime`, off `5894e06`.

This is an overseen sprint. It ran as karc leg `kdeskdash-0c00ad` on kai, and the
overseer's review happens on the proposal thread.

## Premise check

- **The target board drifted, and the proposal had already corrected it.** The WI
  says rpidash2, but the speaker is on rpidash3. It enumerates as card
  `UACDemoV10`, and is now index **1** where it was index 3 in August. That is the
  WI's own argument for addressing it by `CARD=` name, proven by the move. Mixer
  `PCM` sits at 44/147 (30%), as before. `aplay` is present, and so is
  `/proc/asound/UACDemoV10`.
- **The signal exists.** libkdash has `KDASH_CLAUDE_DISP_BLOCKED` and the counted
  reader `kdash_claude_sessions()`.
- **The WI's design note holds, and it matters more on rpidash3.** The claude feed
  is polled only while the claude mode is active. rpidash3 does not register that
  mode at all, so the chime cannot borrow its handle even in principle. Its env
  file already sets the claude endpoint to `rpi53:6379`, because the comment block
  predates the work-desk modeset.

## What shipped

**Shape: the WI's option 1.** A watcher in the main loop runs on its own libkdash
handle. The mode contract is untouched, and nothing was hoisted.

- `src/chime.c` is the pure core and is host-tested in `tests/test_chime.c`, with
  11 cases:
  - **Edge, not level.** It chimes on the transition into `DISP_BLOCKED`, keyed
    on `host` and `sid`, once per block.
  - **The first complete snapshot primes and never chimes.** A restart or a
    deploy does not sound for a session that was already blocked.
  - **A failed read is not a snapshot.** The glue skips `observe()` on `-1`, so
    the tracked set survives a blip and recovery does not re-chime.
  - **Cooldown, default 30 s.** Sessions that block together sound once. An edge
    swallowed by the cooldown is consumed, not deferred, so the chime never
    becomes a queue of alarms.
  - `AWAITING` and `STALE` never chime, which is the WI's agreed scope.
  - The tracked set is capped at 24. Past the cap it errs toward silence.
- **The sound** is synthesized in the core (`chime_tone_render`): two soft sine
  notes, E6 then B6, a rising fifth. Each has a 5 ms attack and an exponential
  decay, the whole thing is 0.6 s, and it starts and ends at exactly zero, so
  there is no click. It is rendered at the speaker's native S16_LE/48 kHz/stereo.
  The tests pin the volume ceiling and the zero ends.
- `src/chime_watch.c` is the glue:
  - Its handle is on `KDASH_STEM_CLAUDE` with the claude mode's endpoint config,
    and passes `""` rather than NULL for "no AUTH", the sprint-034 trap.
  - It scans every 5 s, the claude mode's cadence, from main.c's 1 Hz block.
  - The player is fork/exec `aplay`. The built-in tone is piped as raw PCM
    through a short-lived writer child, so nothing touches disk and the cross
    sysroot gains no `libasound`. The child closes inherited fds, so `aplay`
    does not hold the DRM or evdev descriptors, and it is reaped with `WNOHANG`.
  - It checks presence through `/proc/asound/<CARD>`, and absence is logged once
    each way.
- **Configuration**: `KDESKDASH_CHIME_DEVICE` (unset means off, and off means the
  handle is never created), `_VOLUME` (0–100, default 30, built-in tone only),
  `_SOUND` (a WAV to play instead) and `_COOLDOWN_S` (default 30).
  - Out-of-range numbers fall back to the default.
  - `test_config` gained two cases.
  - The variables are documented in `deploy/kdeskdash.env.example`, the README
    env table and CLAUDE.md, where the feed handle list is now six.
- `deploy/hosts/rpidash3.env` gained the device line.

### What was chosen, so Ken can change it without a rebuild

The built-in "ding-ding" plays at **30% of full scale**, on top of the card's
mixer at 30%, which was left as found. Edit `/etc/kdeskdash/kdeskdash.env` on
rpidash3, then restart the service:

- `KDESKDASH_CHIME_VOLUME=<0-100>` for louder or softer;
- `KDESKDASH_CHIME_SOUND=/usr/share/sounds/alsa/Front_Center.wav` (or any WAV)
  for a different sound;
- `KDESKDASH_CHIME_COOLDOWN_S` for the gap between chimes.

The card mixer, `sudo amixer -c UACDemoV10 sset PCM <n>`, also scales a WAV.

## Proof (triggered, on rpidash3)

The env line was installed on the board by whole-file replace, with the md5
verified. The board's file had differed from the committed one only in comments.
`just push-dev rpidash3` then put the branch build on the board (a dev build,
`0.27.0-18519c4-dirty`; the ship's deploy replaces it). The test session was
published by the feed's own contract from kai, as `HSET
claude:session:kai:chime-test-wi1096-<ts> status blocked ts <now> …`, with a 120 s
TTL and deleted afterwards.

- **It played.** `chime: session BLOCKED — playing built-in tone on
  plughw:CARD=UACDemoV10,DEV=0` was logged at 18:49:53.12 PST. A 50 ms sampler on
  the Pi then saw `/proc/asound/UACDemoV10/pcm0p/sub0/status` go `state: RUNNING`
  at 18:49:53.14, with `owner_pid` 96809 (the aplay child) and `hw_ptr` advancing
  from 864 to 3744 frames. That is evidence the PCM moved samples to the USB
  device, not that anyone heard it. Ken will hear it the next time a session
  blocks.
- **Edge held.** The session stayed blocked for about 25 s and produced one chime.
- **Clean.** There was no `aplay failed` line, no zombie, and the service stayed
  active.
- **Absent card.** With `CARD=NoSuchCard` and a restart, the board logged `ALSA
  card NoSuchCard not present — staying silent until it appears` once and started
  normally. A blocked session published afterwards produced no play line and no
  second log line. The real env was then restored (md5 verified) and restarted.
- **Unconfigured.** This path is unit-level only (`test_config`, and the init
  logs `chime: off`). rpidash2 was not touched, and it picks this up at the
  ship's deploy with no env line, so it stays silent.

## Deployed

Shipped as PR #54, squash `2dcd7f3`. The overseer cleared it on proposal korg:4425
(comment 4486).

- **`deploy-panels`**: `just publish` gave **`0.27.0-2dcd7f3`** (and `latest`).
  `just deploy` pinned both boards to it, and `just versions` reports each board
  on exactly that version.
  - **rpidash2**: the unit is active. It logs `chime: off (KDESKDASH_CHIME_DEVICE
    unset)`, which is the unconfigured path, now proven live. A `kddss` frame
    was taken.
  - **rpidash3**: reachable. This took it off the sprint's push-dev build. The
    unit is active and a `kddss` frame was taken. **Smoke test on the published
    bytes:** a fresh BLOCKED test session, published to rpi53 and then deleted,
    logged `chime: session BLOCKED — playing built-in tone on
    plughw:CARD=UACDemoV10,DEV=0` at 18:54:27 PST, with no `aplay` error.
- **Env refresh on rpidash3 (overseer ruling)**: `/etc/kdeskdash/kdeskdash.env`
  had carried comments from before sprints 037/040. I replaced it whole with the
  committed `deploy/hosts/rpidash3.env`. A settings-only diff showed the two
  identical, so no behaviour changed. The md5 `2dec043e…` matches on both sides,
  the unit restarted active, and the chime is on.
- **`recipe: publish-publisher`**: nothing to publish. `kdeskdash-publisher
  2.4.0-c44430a` is already in the store, because this sprint didn't touch the
  publisher.
