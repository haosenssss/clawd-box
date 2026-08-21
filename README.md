<div align="center">

<img src="assets/hero.png" width="720" alt="Clawd Box">

# Clawd Box

<sub><a href="README.zh-CN.md">中文</a></sub>

</div>

---

## What's on the screen

<img src="assets/screen.png" width="290" align="right" alt="what the board actually shows">

The image on the right is a straight 480×480 dump of what the board displays. The hero
shot above it is a concept render, and the real enclosure is a good deal plainer than that.

The row of dots along the top tracks subagents spawned by the current project, filled once
they finish and hollow while they are still running. Clawd sits in the middle and its pose
tells you which state the project is in, which the next section covers. The line under the
project name says what it is doing right now, picked at random from a list of verbs. The
three bars at the bottom are the 5-hour limit, the 7-day limit and context usage, each with
its own reset countdown on the right.

When several projects are running the display cycles between them, and if only one of them
is busy it stays on that one until something changes. Finishing a turn also plays a short
chime.

<br clear="right">

## Five states

<table>
<tr>
<td width="20%" align="center"><img src="assets/dj.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/done.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/waiting.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/idle.gif" width="150" alt=""></td>
<td width="20%" align="center"><img src="assets/sleeping.gif" width="150" alt=""></td>
</tr>
<tr>
<td align="center"><b>Working</b><br><sub>headphones on, scratching</sub></td>
<td align="center"><b>Finished</b><br><sub>jumps, throws confetti</sub></td>
<td align="center"><b>Needs you</b><br><sub>holds up a lightbulb</sub></td>
<td align="center"><b>Idle</b><br><sub>looks around</sub></td>
<td align="center"><b>Asleep</b><br><sub>snot bubble</sub></td>
</tr>
</table>

Each pose maps to one project state, so you can tell them apart at a glance without
reading anything on the screen.

## Getting started

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/),
[bun](https://bun.sh), and a Waveshare ESP32-S3-Touch-LCD-4B.

```bash
git clone https://github.com/haosenssss/clawd-box && cd clawd-box
```

Flash the firmware:

```bash
cd firmware
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem* flash
```

The board has two USB ports and either one will carry data.

Install the host daemon once and it starts automatically on every login after that:

```bash
./host/launchd/install.sh
```

Then point Claude Code's hooks and status line at it in `~/.claude/settings.json`:

```jsonc
{
  "statusLine": {
    "type": "command",
    "command": "bun run /path/to/clawd-box/host/bin/statusline.ts"
  },
  "hooks": {
    // UserPromptSubmit / Stop / Notification / SubagentStart / SubagentStop
    // all point at the same script. async must be true because these run on the
    // critical path of the conversation.
    "Stop": [{ "hooks": [{
      "type": "command",
      "async": true,
      "command": "bun run /path/to/clawd-box/host/bin/hook.ts"
    }] }]
  }
}
```

Plug the board in and the display will start following along. The daemon logs to
`/tmp/clawdbox-daemon.log`.

## How it's built

<div align="center">
<img src="assets/arch.svg" width="760" alt="architecture">
</div>

The Mac side only reads the session registry, checks that those processes are still alive
and catches the input the hooks hand it, then passes messages along. Everything else runs
on the board, including session state, animation selection, paging and rendering.

Messages replace state rather than accumulating as events: each one overwrites a session's
entry outright, so memory on the board is the session count times a fixed size and stays
flat no matter how long it runs.

There are two sources of data. One is the rate-limit numbers Claude Code has already
computed for its status line, the other is its hook events. Both are read locally and
neither adds a network request.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3 · 16 MB flash · 8 MB PSRAM |
| Display | ST7701 RGB 480×480 |
| Touch | GT911 |
| Audio | ES8311 + amplifier |
| Expander | TCA9554 (panel reset, backlight and SPI chip select all sit behind it) |

## Development

Two tools run without the board attached.

The first previews animation by compiling the firmware's own `clawd.c` natively and
writing frames out. Every GIF and screenshot in this README came from it:

```bash
cd firmware
clang -O2 -I main -o /tmp/prev ../tools/preview/preview.c main/sprite/clawd.c -lm
/tmp/prev /tmp/out.raw
```

The second tests the rotation and preemption logic. Time is passed in as an argument, so
nothing waits on a real clock:

```bash
./tools/logic-test/run.sh
```

Host side:

```bash
cd host && bun test
```

---

Unofficial project, not affiliated with Anthropic. Code is [MIT](LICENSE) licensed.
