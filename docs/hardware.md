# Building the hardware

Everything you need to build a kdeskdash panel: what to buy, what to print, and
how it goes together. The result is a self-contained desk unit — a 1920×440
touch panel and a Raspberry Pi in one printed case, with a single power lead and
an ethernet cable out the back.

This is the *build* side. This page covers what to buy and how to print it; the
step-by-step build with photos is in **[Assembling the panel](assembly.md)**.
Once it is assembled and imaged, the [README](../README.md) covers the software:
cross-compiling, deploying, and the boot-to-dashboard service.

## Bill of materials

Links are Amazon US and were verified working when this was written; they are
not affiliate links. Several items come in packs larger than you need, which is
called out below so you know what you are actually buying.

| Item | Qty | Notes |
|---|---|---|
| [GeeekPi 11.26" 1920×440 HDMI capacitive touch screen](https://www.amazon.com/dp/B0F6NPCX1V) | 1 | The panel the case is designed around. ILITEK touch controller. |
| Raspberry Pi 5 or Pi 4 Model B | 1 | See [below](#which-pi). No direct link — listings churn too fast to keep one current. |
| [CanaKit 45W USB-C PD power supply](https://www.amazon.com/dp/B07H125ZRL) | 1 | Recommended for the Pi 5. A Pi 4 can use something smaller, but this works for both. |
| [Silkland USB-C 90° right-angle adapter](https://www.amazon.com/dp/B0CNGFZ1JD) | 1 | **Sold as a 2-pack.** This specific adapter is known to clear the case — others may not fit. |
| [M3 × 8 mm socket-head cap screws](https://www.amazon.com/dp/B07CMQ1SQH) | 6 | Sold 100 to a bag, so you will have plenty of spares. |
| [M3 heat-set inserts, 4 mm](https://www.amazon.com/dp/B0FD88XTV4) | 6 | Sold as a 300-piece kit spanning M3 3–10 mm, and it includes the soldering-iron tip you need to set them. |
| [3M VHB 5952 acrylic foam tape, 3/4" × 15 yd](https://www.amazon.com/dp/B0016HM7SE) | optional | Only if you hit the tape problem in [step 3](assembly.md#3-mount-the-pi-to-the-monitor). It has to be real VHB — see [the note there](assembly.md#a-word-on-the-tape). Sold in a 15-yard industrial roll, which is vastly more than this job needs, so it is not worth buying solely for this unless you have another use for it. |

You will also need a soldering iron (to set the inserts) and a hex driver for
the M3 screws.

### Which Pi

Either works, and the binary is generic aarch64 — board choice does not fork the
build. The two panels in service are a Pi 5 (8GB) and a Pi 4 Model B Rev 1.5
(8GB). On the Pi 4's A72 cores the Game of Life composite render is the only
mode that works noticeably harder; everything else is indistinguishable. Search
links, since product listings move around:
[Raspberry Pi 5](https://www.amazon.com/s?k=raspberry+pi+5) ·
[Raspberry Pi 4 Model B](https://www.amazon.com/s?k=raspberry+pi+4+model+b).

## Printing the case

Three STLs in [`stl/`](../stl/):

| File | What it is |
|---|---|
| [`deskdash_case.stl`](../stl/deskdash_case.stl) | The main case body |
| [`lside_deskdash_case.stl`](../stl/lside_deskdash_case.stl) | Left end cap |
| [`rside_deskdash_case.stl`](../stl/rside_deskdash_case.stl) | Right end cap |

**The main body is ~277 mm long, so it needs a large-format printer.** Print it
standing on its right end; no supports are needed in that orientation. All three
parts fit on one plate:

![All three case parts arranged on a Bambu H2D build plate — the main body standing on its right end, with both end caps laid flat](images/Plate-Layout-Bambu-H2D.png)

> **Smaller printer?** [Open an issue](https://github.com/kenhia/kdeskdash/issues/new)
> and I will add STLs that split the main body into three superglue-together
> parts. The parts already exist — they just need indexing features added so
> they align properly, which is not worth doing speculatively.

### Settings — print it in PLA

**Print in PLA.** The case in service was printed in **matte black PLA** on
stock settings — 15% infill, nothing special — in about 9.5 hours, and it has
held up fine. There is real heat in this enclosure, with a Pi and a monitor
running inside it, but in practice it has not turned out to be enough heat to
worry about.

### A note about ABS

**ABS is not recommended at this time.** You *can* get a working part out of
these STLs — scale every piece by **100.545%** in the slicer and the shrinkage
comes out right — but that only fixes the dimensions. Even with decently tuned
settings I am still getting visual defects, and those are a result of the
**model**, not the slicer. No profile tuning is going to make them go away.

So the scale factor is offered as a way to make an ABS case that *fits*, not as
a recommendation to print one. I am giving more thought to how to get an ABS
print that looks good enough for a project like this. Perfection is not the
goal — but an obvious defect you have to look at every day is a different
thing, and that is what needs resolving before ABS gets a recommendation here.

Where the number comes from: ABS shrinks as it cools, and over a 277 mm part
that adds up. The first ABS body came out roughly 1.5 mm short, which is enough
to matter on a part whose whole job is to hold a monitor in a groove. 100.545%
is that shortfall compensated.

The defect I most want gone is a **blind overhang** inside the case, which PLA
bridges acceptably and ABS sags on. The intended fix is an access hole in the
back so the overhang can be closed up properly rather than bridged at all —
a model change, which is why it has to wait for a revised model rather than a
better profile.

Updated STLs will land once the revised model has been printed and test-fitted.

#### The ABS settings so far

The profile the trial prints used — recorded so it is not lost, not a
recommendation to go print one now. On a **Bambu H2D** with **Bambu ABS
Black**, starting from the `0.20mm Standard @BBL H2D` profile:

| Setting | Value | Why |
|---|---|---|
| Wall loops | `4` | More solid plastic around the heat-set inserts |
| Bottom layers | `5` | Same |
| Enable clumping detection by probing | checked | Bambu recommended, wish they would just make this the default |

Estimated print time: **12 h 9 m**.

Note what did *not* change: infill stays at the profile default. It is tempting
to reach for 40% gyroid when moving to ABS, but the PLA case was already strong
enough in bulk, and ABS at the same parameters will be stronger still. The two
settings that were raised are both about **material around the heat-set
inserts** — that is where this part actually sees load, since six threaded
inserts carry the end caps and the whole assembly hangs off them. Adding walls
and bottom layers puts plastic exactly there; adding infill would mostly add
hours for nothing — just changing to 40% infill with gyroid would add over 12
more hours to the print!

## Assembly

The build itself, step by step and with photos, is its own page:
**[Assembling the panel](assembly.md)**.

Roughly: set the six heat-set inserts and let them cool, image the Pi, mount the
Pi to the monitor, close one end of the case, route and connect the cables, slide
the monitor in, and close the other end. Budget an evening.

## Networking

Both panels in service use **ethernet**, chosen for reliability rather than
because WiFi was tried and found wanting. A dashboard is only useful if it is
always current, and a panel showing stale data is worse than one showing none.

WiFi should work. kdeskdash polls Redis about once a second per enabled feed,
each feed is independently failure-isolated, and a dropped connection backs off
and retries without stalling the main loop. A brief outage should surface as
data that goes stale and then heals.

It is untested over the long haul, though. If you run a panel on WiFi, reports
are wanted in
[issue #25](https://github.com/kenhia/kdeskdash/issues/25) — especially whether
it survives idle overnight and recovers after an AP reboot.

## Next

Plug it in and deploy kdeskdash to it — see
[Build](../README.md#build-cross-compile-from-a-dev-host) and
[Service](../README.md#service-boot-to-dashboard) in the README.
