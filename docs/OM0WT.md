# OM0WT's additions

What this fork (om0wt/OpenX6X00) has over Guru-RF's OpenX6X00. Upstream's docs (THEMES.md, MANUAL.md)
are left as they are, so their changes merge without a conflict; this page is ours.

## Theme: KX3Lovers Plus

Settings -> Display -> INTERFACE -> Theme, or `"base": "KX3Lovers Plus"` in a theme file (THEMES.md).

Amber glass laid out as the LCD in Elecraft's KX3 owner's manual:
- **On the display:**
  - the bar graph with the S, SWR/CMP and RF/ALC scales and CWT;
  - the filter graphic with NTCH;
  - VFO A in seven segments, with VOX, ATU, RIT, XIT, SPLT, AGC, ATT, PRE, NB, NR and XFIL under it;
  - VFO B in fourteen segments;
  - the lock, A, TX and B, and the mode icons on the right.
- **Under it:** a spectrum strip (in CW the decoded text beside it, in fourteen segments), the power, IF, the time and the voltage.
- **The buttons:** one line, without a frame.

The spectrum strip is shown whatever Settings -> Display say about the spectrum.

## Settings -> TX Cal -> Auto, all bands

On a base with a band output gain (Xiegu's r13 and newer), this sets the band output gain of every band from 160 m to 6 m. It aims for the radio's own power meter to read the PWR setting.

It transmits on every band: **into a dummy load only**.
- **Each band:** FM carriers of 2.5 s, up to 7 a band.
- **Where the limiter holds the power:** that band is left at its full power.
- **It stops on:**
  - an SWR over 2;
  - no power out;
  - Stop;
  - a band change;
  - anything else transmitting.
- **Starting:** press once and the note says it will transmit, then press again within 5 s.
- **The config socket's commands:** `txcal_start`, `txcal_stop` and `txcal_get` (src/tx_autocal.h).

## Fixes

- Tuning a VFO across a band edge no longer changes the other VFO: the inactive one keeps its frequency, mode, ATT, PRE and AGC.
- Settings pages scroll to their end when the last control is disabled (Tailscale's Log out).
- In the frequency strip, NR, NB, DNF, RIT and XIT sit left of the locator (VISION: over it), not in its way.

## Merging upstream

Our code lives in its own files:
- lcd_panel.c and .h;
- theme_kx3.inc;
- tx_autocal.cpp and .h;
- this page.

Upstream's files only call into them, one line each, marked with a comment naming our file (`theme_kx3.inc`, `tx_autocal.cpp`, `KX3Lovers Plus`, `OM0WT`).

A conflict, when one still comes, is at one of those lines: keep upstream's side and put the call back.
