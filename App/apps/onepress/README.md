# OnePress: type messages with one key press per letter

`OnePress` types a message on the keypad with one press per letter, and the
whole sentence picks the words: `4663` is "good", "home" or "gone", and the
words around it say which. It is an APRS messenger that sits beside the
radio's normal voice use, on repeaters or simplex, and listens for answers
while you type or talk:

- `OP APRS` sends it as an APRS message to a callsign (Bell 202 AFSK, as APRS TX);
- `OP Inbox` shows the APRS messages to your callsign that the radio heard in
  the background while these apps were open, and replies to them;
- `OP Location` keeps 4 locations, one of which OP APRS can send after a message.

The decoder is [OnePress](https://github.com/JamesDavid/OnePress-Keyboard)
(`App/onepress`, PolyForm Noncommercial 1.0.0: the OnePress edition is for
noncommercial use). It stays resident and serves the apps through API level 4
(`APP_CAP_ONEPRESS`), with the APRS message receiver at level 5
(`APP_CAP_APRS_INBOX`); its word model, 13,000 words including CQ, QTH, QSO, tnx,
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
   for the decoder; and without VOX, the PMR and GMRS/FRS/MURS TX lock
   profiles, the RX/TX log and the K-plug serial port, which make room for the
   receiver. After flashing it over another edition, check `F Lock` (hidden
   menu): profiles after `438` moved up two places. Put it in a Multiboot slot
   to keep your usual edition.
2. Build the apps, `./compile-app.sh onepress opaprs opinbox oploc`, and
   install `OnePress.app`, `OPAPRS.app`, `OPInbox.app` and `OPLocation.app`
   with UV Studio.
   Reinstall them all after flashing a new firmware.
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
| `*` | take the completion shown (`* TAKES IT`), else the next guess for the selected word (best first, by the whole sentence) |
| UP / DOWN | select the word before / after, to fix it with `*` (`* FIXES`); the rest of the message re-decodes around the fix. DOWN past the last word, typing or deleting goes back to the end |
| F | `ABC`: spell exactly with multi-tap (call signs, names); F again: back |
| EXIT | delete the last key; held, delete words; on an empty message, quit |
| MENU | `1` APRS message (with no message: its settings), `2` inbox, `3` clear, `4` locations |
| PTT | send it as an APRS message |

The bottom line shows the length against the 67 characters of an APRS
message. Call signs typed in `ABC` (`ke7aqa`) show in capitals. The message
stays when you leave the app, until the radio is turned off, and after it is
sent, so PTT sends it again; `MENU` `3` clears it. To leave the apps: `MENU`
`3`, then `EXIT`. The apps light the red LED while they transmit.

## Receiving APRS messages

While OnePress, OP APRS, OP Inbox or OP Location is open, the radio decodes APRS on
the RX VFO in the background and keeps the messages addressed to your callsign
and SSID set in OP APRS (`KE7AQA-7`), or to the bare callsign (`KE7AQA`). A
new one beeps and shows `1 MSG` at the top of OnePress; `MENU` `2` reads it.
The last 3 are kept in external flash (the sector after the word model), read
or not, so they are still there after the radio is turned off. The speaker
stays off in the apps; the receiver pauses while an app transmits.
To read all APRS traffic, use the APRS RX app.

It keeps listening on the main screen too, from power on once a station has
been set (`ALWAYS` in OP Inbox; `F` there turns it off and on). There it uses
the radio's own audio: it decodes what the squelch lets through, on the RX VFO
(both, with dual watch), and pauses while the radio transmits or sleeps (power
save). A new message gives three short beeps and an envelope with the number
unread on the status bar; OnePress `MENU` `2` reads them. Its acks go out from
the main screen as well. Listening costs about 8% of the CPU, and some
battery while the squelch is open.

A message that carries a number (`{12`: most radios and apps add one) asks
for an acknowledgement, and the sender repeats it until one gets through. The
radio sends it by itself, `ack12` from the address the message was sent to,
about 1.5 s after the message once the channel is quiet, on the frequency
it was heard on and by the path set in OP APRS; a repeat is acknowledged again but kept once. The
red LED lights for the half second it takes. Answers to your own messages
(`ack` / `rej`) are not shown as messages: OP APRS shows them.

## OP Inbox

| Key | Action |
|---|---|
| UP / DOWN | the next / previous message (newest first) |
| MENU or PTT | reply: back to OnePress, with the sender as OP APRS's addressee |
| `*` | the receiver's diagnostics line on / off |
| F | listening on the main screen too (`ALWAYS`), or only in these apps |
| EXIT | back to OnePress |

The line under the messages says whom the radio listens for. The diagnostics
line, `9600/S 12PKT A1850-2250 B2010`, gives the audio samples taken a second
(about 9600), the packets decoded from any station since the start, the audio's
lowest and highest ADC code over the last second (mid-scale 2048: a strong
signal changes it), and the battery's ADC code. If messages do not arrive,
check that the radio hears the sender on the main screen first.

## OP APRS

`SRC-SSID>APZOP,path::ADDRESSEE:text{12`. The number asks the addressee for an
acknowledgement; sending the same message again keeps it, so the addressee
shows it once. When the answer is heard the bottom line says `DELIVERED` (or
`REJECTED`); until then `SENT 1 NO ACK YET`, and PTT sends it again. iGates
forward it to APRS-IS, so the addressee sees it on aprs.fi or in an APRS app
too. The callsign, SSID and path set here are also the ones the receiver
listens for and acknowledges with.

A message to a station in the inbox (a reply) goes out on the frequency its
last message was heard on, simplex, even if the VFO has moved since: the
frequency shown is that one, and the `FROM` line ends in `REPLY`. Messages
keep the frequency in flash too. Otherwise it goes out on the TX VFO.

| Key | Action |
|---|---|
| PTT | send (only PTT: a MENU pressed once too often must not transmit); off an APRS frequency, a warning first (`NOT APRS: 144.390`), PTT again sends |
| 5 | the location sent after the message: none, then each saved one (`@1` on the `FROM` line) |
| 6 | OP Location, to enter the locations |
| 1 | edit the addressee |
| 2 | edit your callsign (empty: the one in the boot message) |
| 3 / 4 | SSID / path: `DIRECT`, `WIDE1-1`, `WIDE1-1,WIDE2-1` |
| EXIT | back to OnePress |

Editing: 2-9 multi-tap A-Z and the digit (or hold the key for the digit), 0
and 1 digits (1 again: `-`), EXIT deletes, MENU done. Defaults: SSID 7,
`WIDE1-1`. Set the VFO to 144.390 MHz (North America) or 144.800 MHz
(Europe) first. The tone level is APRS TX's default, which Yaesu and UV-K1
receivers decoded first try.

A location attached with `5` follows the message in the same transmission,
as a position report: `SRC-SSID>APZOP,path:!4903.50N/07201.75W[` (a person
on the map). APRS messages cannot carry a position themselves; the addressee's
APRS app and aprs.fi show this one next to the message.

## OP Location

Four slots for places you send from: home, a club, where you are now. They are
kept over power off with the inbox (external flash).

| Key | Action |
|---|---|
| UP / DOWN | the slot |
| MENU | edit it |
| 5 | send it after OP APRS's messages (`SENT`); again: none |
| EXIT | back to OP APRS |

Editing: type the digits in order, degrees, minutes and hundredths of a minute,
latitude then longitude: 49 03.50 N, 072 01.75 W is `4903500720175`. `*`
switches N / S, `F` E / W (W to start with), EXIT deletes a digit (with none
left: the slot is cleared), MENU done. A GPS or a map app gives degrees and
minutes; from decimal degrees, the minutes are the decimals times 60.

## With voice repeaters

APRS has a frequency of its own (144.390 MHz in North America), so for messages
while you use a repeater, put one VFO on the repeater and the other on 144.390,
with dual watch on. The receiver listens on whichever VFO the radio is on;
dual watch stops on the one that hears something, so a packet on 144.390 is
caught as long as the repeater is quiet at that moment. Inside the apps the
radio stays on the selected VFO (no dual watch there): select the 144.390 one
while you type, or OP APRS warns `NOT APRS` before sending through the
repeater. Voice works as usual
(PTT on the main screen pauses the receiver). Acks and replies go out on the
frequency the message was heard on, simplex, whichever VFO is selected for
transmit, so they never go out through the repeater by mistake.

## Frequencies

Nothing is enforced (band plans differ from country to country), but OP APRS
warns before sending off an APRS frequency, `NOT APRS: 144.390`, and sends on a
second PTT. The APRS frequencies: 144.390 MHz (North America, the main one),
144.800 (Europe), 145.825 (the ISS and satellites) and 445.925 (UHF, some
regions). This also catches two radios that are not on the same frequency.
Keep voice off 144.390: it is for data only.

> [!CAUTION]
> Transmit only on frequencies, power levels and modes your licence allows.

## Testing on a PC

`tools/onepress_sim/build.sh` builds `build/opsim`, which runs the four apps
against a simulated radio: the real resident service and decoder, the model
file as the external flash, keys from a script, the screen as text, and every
APRS transmission decoded back from its tones, FCS checked. For example
`build/opsim onepress-r.bin "6 6 0 6 9 0 9 2 9 0 4 6 6 3 s"` shows "On my way home".
A third argument, `audio.u16@seconds` (`tools/aprs_rx_test/gen.py`), is RX
audio the background receiver hears from that time on. `tools/aprs_rx_test`
also tests the decoder on recorded traffic (`rxtest`) and the inbox in flash,
power cuts included (`storetest`).

## Resources

The decoder takes about 21 KB of flash and 2.1 KB of RAM (1.6 KB of working
memory, a 256-byte read cache, the model's table), plus up to 2.5 KB of stack
while it decodes; `System Info` shows the stack margin left. A key press reads
the model about 2,000 times (32 bytes each, about 16 us at 24 MHz) with the cache. The apps fit the
4 KiB overlay: OnePress 3.8 KB, OP APRS 4.0 KB, OP Inbox 1.6 KB, OP Location 1.6 KB.

The receiver (`App/onepress/aprs_rx.c`, the APRS RX app's demodulator, and
`App/app/aprs_bg.c`, with the inbox store `App/app/aprs_store.c` and the ack
sender, the locations and the main screen) takes about 6.3 KB of flash and
0.9 KB of RAM. TIM15
starts each ADC conversion of the RX audio, 9600 a second, so the samples stay
evenly spaced however late the interrupt runs; the interrupt uses about 8% of
the CPU (1,800 cycles at most of the 5,000 a sample allows).
