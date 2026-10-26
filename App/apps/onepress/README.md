# OnePress: type messages with one key press per letter

`OnePress` types a message on the keypad with one press per letter, and the
whole sentence picks the words: `4663` is "good", "home" or "gone", and the
words around it say which. Two sender apps put the message on the air:

- `OP APRS` sends it as an APRS message to a callsign (Bell 202 AFSK, as APRS TX);
- `OP CW` sends it in Morse, as a tone over FM (MCW) or with the carrier keyed.

The decoder is [OnePress](https://github.com/JamesDavid/OnePress-Keyboard)
(`App/onepress`, PolyForm Noncommercial 1.0.0: the OnePress edition is for
noncommercial use). It stays resident and serves the apps through API level 4
(`APP_CAP_ONEPRESS`); its word model, 13,000 words including CQ, QTH, QSO, tnx,
repeater, node and the like (`onepress-r.bin`, 491 KB), sits in external flash
at 0x162000. Against a classic one-word-at-a-time predictive keypad with the
same words (`tests/baseline.js` in the OnePress repository), on text it never
saw, the words neither knows spelled out:

| | Classic keypad | OnePress |
|---|---|---|
| Radio messages: words right | 78.9% | **96.0%** |
| Everyday text messages: words right | 90.0% | **94.9%** |
| Everyday text messages: punctuation right | 19% | **47%** |

3.6% of everyday words are not among its 13,000: spell those with `ABC`.
`onepress-s.bin` (246 KB, 5,000 words) works too, with more words to spell.

## Install

1. Build and flash the `OnePress` edition: `./compile-firmware.sh OnePress`. It
   is Labs without the spectrum analyser, AirCopy, broadcast FM, Full Watch,
   QR codes, Rescue Ops, the audio scope and the boot logo, which make room
   for the decoder. Put it in a Multiboot slot to keep your usual edition.
2. Build the apps, `./compile-app.sh onepress opaprs opcw`, and install
   `OnePress.app`, `OPAPRS.app` and `OPCW.app` with UV Studio.
3. Upload the model over the USB-C cable, from the main screen:
   `python tools/onepress_upload.py COM5 onepress-r.bin` (`pip install pyserial`;
   the model is in the OnePress repository's `model/` folder). Uploading again
   only rewrites what changed.

## Keys (OnePress)

| Key | Action |
|---|---|
| 2-9 | a letter: press each key once, whichever of its letters you mean |
| 0 | a space |
| 1 | punctuation (the decoder picks `.` `,` `?` `!`, or an apostrophe inside a word) |
| a digit key, held | the digit itself |
| `*` | take the completion shown (`* TAKES IT`), else the next guess for the last word |
| DOWN / UP | the next / previous guess for the last word (best first, by the whole sentence) |
| F | `ABC`: spell exactly with multi-tap (call signs, names); F again: back |
| EXIT | delete the last key; held, delete words; on an empty message, quit |
| MENU | send: `1` APRS message, `2` CW, `3` clear |
| PTT | send the way you sent last time |

The bottom line shows the length against the 67 characters of an APRS
message. Call signs typed in `ABC` (`ke7aqa`) show in capitals. The message
stays when you leave the app, until the radio is turned off; after a sender
sends it, it is cleared.

## OP APRS

`SRC-SSID>APZOP,path::ADDRESSEE:text`, with no message number (this app does not
receive, so it asks for no acknowledgement). iGates forward it to APRS-IS, so
the addressee sees it on aprs.fi or in an APRS app too.

| Key | Action |
|---|---|
| PTT or MENU | send |
| 1 | edit the addressee |
| 2 | edit your callsign (empty: the one in the boot message) |
| 3 / 4 | SSID / path: `DIRECT`, `WIDE1-1`, `WIDE1-1,WIDE2-1` |
| EXIT | back to OnePress |

Editing: 2-9 multi-tap A-Z and the digit (or hold the key for the digit), 0
and 1 digits (1 again: `-`), EXIT deletes, MENU done. Defaults: SSID 7,
`WIDE1-1`. Set the VFO to 144.390 MHz (North America) or 144.800 MHz
(Europe) first. The tone level is APRS TX's default, which Yaesu and UV-K1
receivers decoded first try.

## OP CW

| Key | Action |
|---|---|
| PTT or MENU | send; EXIT stops |
| UP / DOWN or 2 / 8 | speed, 5-40 WPM (18) |
| 3 / 9 | tone, 400-1200 Hz (700) |
| 1 | `MCW` (a tone over FM: any FM radio hears it) / `CARR` (the carrier keyed too, for a CW receiver) |
| EXIT | back to OnePress |

> [!CAUTION]
> Transmit only on frequencies, power levels and modes your licence allows.

## Testing on a PC

`tools/onepress_sim/build.sh` builds `build/opsim`, which runs the three apps
against a simulated radio: the real resident service and decoder, the model
file as the external flash, keys from a script, the screen as text, and every
APRS transmission decoded back from its tones, FCS checked. For example
`build/opsim onepress-r.bin "6 6 0 6 9 0 9 2 9 0 4 6 6 3 s"` shows "On my way home".

## Resources

The decoder takes about 21 KB of flash and 2.1 KB of RAM (1.6 KB of working
memory, a 256-byte read cache, the model's table), plus up to 2.5 KB of stack
while it decodes; `System Info` shows the stack margin left. A key press reads
the model about 2,000 times (32 bytes each, about 16 us at 24 MHz) with the cache. The apps fit the
4 KiB overlay: OnePress 3.0 KB, OP APRS 2.7 KB, OP CW 1.5 KB.
