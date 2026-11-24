<div align="center">

<h1>MMCTool for M5Stack Cardputer &amp; Cardputer-Adv</h1>

<p><b>🇬🇧 English</b> | <a href="README.ru.md">🇷🇺 Русский</a> | <a href="README.es.md">🇪🇸 Español</a></p>

<p>
  <a href="https://platformio.org/"><img src="https://img.shields.io/badge/PlatformIO-ESP32--S3-orange?style=flat-square" alt="PlatformIO"></a>
  <a href="https://github.com/bmorcelli/Launcher"><img src="https://img.shields.io/badge/M5Launcher-Compatible-brightgreen?style=flat-square" alt="M5Launcher"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-blue?style=flat-square" alt="GPL-3.0"></a>
</p>

<p><i>Pocket 6-voice polyphonic synth, guitar, harp, piano &amp; breakcore/tekk drum machine for Cardputer and Cardputer-Adv. Runs on 0 MB PSRAM with 200% amp boost and SD WAV recording.</i></p>

<p><a href="../../releases/latest"><b>⬇️ Download Latest .bin (M5Launcher &amp; FullFlash 0x0)</b></a></p>

</div>

## What is this?

I wanted a real pocket instrument on my Cardputer-Adv where you can actually hold chords, play real tracks with proper note names, mess with reverb/echo, and record ideas to SD without touching a PC. Most existing apps were monophonic beeps, so I built **MMCTool**.

Single 2-in-1 `.bin` works on both **M5Stack Cardputer-Adv** (with ES8311 DAC + 3.5mm jack) and original **M5Stack Cardputer**.

## What it can do

- **10 Real-Time DSP Instruments (hold 1.3s to switch so you don't hit them by mistake):**
  - `1` **BASS** - punchy bass guitar with sub and drive
  - `2` **E-GTR** - electric guitar with 3-level overdrive (`CLN / CRN / HVY`)
  - `3` **LYRE** - crystal plucked harp (Karplus-Strong string physics)
  - `4` **SYNTH** - polyphonic synth (`SAW / SQR / TRI / PAD / Phonk BELL` via `7`)
  - `5` **DRUMS** - 3 switchable drum kits via `7`: **AMEN** (breakcore, ringing crash, snare roll), **TEKK** (hardtekk/gabber/piep kicks + rave siren), **PHNK** (808 trap + cowbell + laser)
  - `6` **DPAD** - MPC mode: top row = instant emotional chords (`Am, F, C, G, Dm, Em, E7, A7`), mid row = matching 808 bass, bottom row = beats
  - `Esc` **PIANO** - acoustic hammer piano (auto-switches to real Piano key layout)
  - `Tab` **REESE** - heavy detuned DnB / Sewerslvt Reese bass
  - `Fn` **CHAIN** - 4-oscillator Chainsaw Supersaw lead for EDM / Hyperpop
  - `Ctrl` **ACID** - resonant TB-303 acid squelch synth
- **Real Music Notation & Orientation:**
  - Actual note names (`C, C#, D, F#...`) inside every pad instead of random numbers
  - Root note highlighted with a double ring, sharps (`#`) styled like black keys, octave numbers on the left
  - Live musical key in the top bar (`K:Am`, `K:F#m`) + real-time chord detector (`[Am] A3 C4 E4`)
  - Instant octave shift `-3..+3` with `-` / `=` or side button `G0` (`1x` up, `2x` down)
- **5 Playable Layouts (hold `9` / `0`):** `MAJ` (Genshin Lyre style), `MIN` (emotional minor), `PNO` (real piano with `#` sharps on top), `BIG` (wide double-keys so small buttons are easy to hit), `CHD` (1-key strummed chords).
- **Live FX & 200% Volume:** Reverb (`O`), Stereo Ping-Pong Echo (`P`), Vibrato/Tremolo (`K`), Filter (`L`), 1-tap Sustain lock (`M`), and 30-step volume up to `200%` (`[` / `]`) with automatic ES8311 hardware amp boost.
- **SD WAV Recorder & Overdub Player:**
  - Records clean stereo WAVs to `/MMCTool_Rec/` without touching your music folders
  - Hold `\` to start/stop recording, hold `Enter` to play/stop (you can play live over your recording!), **Tap + Hold `Enter`** to open the Recordings Menu, `Del` to force stop playback.
- **GUIDE Mode (Synthesia trainer):** Hold `/` to highlight notes step-by-step from `/MMCTool_Songs/*.txt`. Includes `index.html` browser converter for turning `.mid` / `.mp3` / `.wav` into guide files.
- **4 Styled Themes + 4 Unique Boot Splashes (saved in NVS):** `RETRO 84`, `AMBER CRT`, `CYBER 2077`, and `GHOST WHITE`. Switch with `G0` x3 or hold `'`.

## Install

- **`MMCTool_M5Launcher.bin`** - drop on microSD and install via [M5Launcher](https://github.com/bmorcelli/Launcher) by @bmorcelli.
- **`MMCTool_FullFlash_0x0.bin`** - direct USB flash at offset `0x0` via M5Burner or Web ESPTool.

## Quick Controls

| Key | Action |
| --- | --- |
| `Q..I` / `Aa..J` / `Opt..N` | Straight 3x8 (24 pads) play grid with dissolving ripple waves |
| Hold `1`..`6` / `Esc` / `Tab` / `Fn` / `Ctrl` | Switch instrument (1.3s hold lock) |
| Hold `7` | Drum Kit (`AMEN/TEKK/PHNK`), Synth Wave, or Drive (`CLN/CRN/HVY`) |
| Hold `8` / Hold `9`,`0` | Toggle Visualizer (Ripple Grid / CRT Scope) / Switch Layout |
| `-` / `=` or `G0` (1x / 2x) | Shift Octave `-3..+3` |
| `[` / `]` | Volume 0..200% (30 steps + auto ES8311 boost) |
| Hold `O` / `P` / `K` / `L` | Reverb / Echo / Mod (Vib/Trem) / Filter (Brit/Warm/LoFi) |
| `M` / `Space` | Sustain lock (1-tap toggle) / Sustain pedal (hold) |
| Hold `\` / Hold `Enter` / `Del` | Record WAV / Play-Stop Rec / Force Stop |
| Tap + Hold `Enter` | Open `/MMCTool_Rec` Recordings Menu |
| Hold `/` | Toggle GUIDE song trainer (`/MMCTool_Songs/*.txt`) |
| `G0` (3x) or Hold `'` | Switch Theme (saved on reboot) |

## Support & Custom Builds

This firmware is free and open-source.  
Unfortunately, bills can't pay themselves 💀

If you like MMCTool or want custom features, bespoke themes, custom drum kits, or tailored firmware for your Cardputer, feel free to reach out:

- USDT (TRC20): TF7kf8QxKoXrHq8ixnaYG5zDXBBuRP5FWb 
- BTC: bc1qef8762lcc72zxz5w6kac4vztpjuw5vxlxaal5c
- **Contact:** Farbender@protonmail.com

## License

GNU General Public License v3.0 (GPL-3.0).
