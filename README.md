# Xvaser LL v2 firmware for STM32F042 based boards

Target hardware:
* [CANtact] - opensource USB-CAN adapter project
* [CANable] - opensource USB-CAN adapter based on CANtact project
* Any other similar boards


Toolchain:
- GNU Arm Embedded Toolchain

FYSETC UCAN (STM32F072)
----

The [UCAN] (STM32F072CBT6) is supported by two extra make targets:

| Target | Clock | Output |
|---|---|---|
| `make ucan` | 8 MHz crystal X1 → PLL ×6 → 48 MHz (preferred) | `build-ucan/kll_ucan_8Mhz.bin` |
| `make ucan-hsi48` | internal HSI48 trimmed by USB SOF (fallback if X1 is dead) | `build-ucan-hsi48/kll_ucan-hsi48.bin` |

Both run CAN, USB and the timestamp timer from the same 48 MHz. The crystal is
preferred because it holds ±50 ppm, while HSI48+CRS only reaches roughly
0.1–0.25%, which is close to the CAN tolerance at 1 Mbit/s. The stock
candleLight firmware ignores X1 and uses HSI48.

Pin map, from `Hardware/UCAN V1.0 SCH.pdf` in the FYSETC/UCAN repo:

| Function | Pin |
|---|---|
| CAN RX / TX | PB8 / PB9 (AF4) |
| LED1 (TX) / LED2 (RX) | PA0 / PA1, active high |
| `CAN_S` (SN65HVD230 Rs; high = standby, 10k pull-down) | PC13, driven low at init |
| BOOT0 | P2 pin 5; bridge to P2 pin 6 (+3.3 V) for ST ROM DFU |

The emulated device reports USB `0bfd:0120` "Kvaser Leaf Light v2". Both of its
serials come from the STM32's 96-bit unique ID, so each adapter is distinct:

- The Kvaser serial (`kv_serial` in sysfs, used by udev naming) is the three ID
  words XORed and mapped to 100000–999999.
- The USB serial string is the full ID as 24 hex digits.

### Flashing over DFU

1. Unplug the UCAN, bridge BOOT0 (P2 pin 5) to +3.3 V (P2 pin 6), plug it in.
   It enumerates as `0483:df11`.
2. Back up the stock firmware before the first flash (the flash is 128 KiB):

       dfu-util -a 0 -s 0x08000000:0x20000 -U ucan-stock-fw.bin

3. Flash:

       dfu-util -a 0 -s 0x08000000:leave -D build-ucan/kll_ucan_8Mhz.bin

   A `get_status` error after "Submitting leave request" is normal; the ROM
   jumps to the new firmware without replying.
4. Remove the BOOT0 bridge, otherwise the next power-up enters DFU again.

To restore the stock firmware, flash the backup the same way.

Once this firmware is running, BOOT0 is no longer needed. The Kvaser
`CMD_RESET_CARD_REQ` command makes the firmware detach from USB and reboot
straight into the ST bootloader: a flag kept in `.noinit` RAM survives the
reset, and `main` jumps to system memory at `0x1FFFC800` before any other
setup. Leaving DFU then boots the application normally. `tools/reset_card.py`
sends that command from Linux: it unbinds `kvaser_usb` and writes through usbfs,
so it needs root.

    sudo tools/reset_card.py <kv_serial>     # then dfu-util -a 0 -s 0x08000000:leave -D <image>

### Validation

`tools/validate_bench.py` runs against a real Kvaser on a two-node bus. It
finds both interfaces by Kvaser serial: `--peer-serial` names the real Kvaser,
and the UCAN is the remaining Kvaser interface unless `--dut-serial` is given.
`--protect-serial` excludes other adapters on the same host. Pass
`--dut-iface`/`--peer-iface` instead when the driver has no `kv_serial`
attribute (the in-tree `kvaser_usb`). Every frame carries a sequence number,
and a case passes only on an exact match with no netdev drops or overruns. Bus
load is measured from the exact on-wire length of each frame, stuff bits
included. The cases (`--cases`, default `abcdghstef`; add `i` when CANlib is
available) are:

- (a) standard/extended frames with DLC 0/3/8
- (b) RTR frames
- (c) 125k/250k/500k/1M
- (d) a 1000-frame back-to-back burst each way at 500k and 1M
- (g) a simultaneous bidirectional back-to-back burst at 500k and 1M
- (h) `cangen` saturation over SocketCAN on both adapters, with load reported
- (i) full-load cases with the real Kvaser driven through Kvaser CANlib
  (`tools/canlib_sender.c`, built against linuxcan), passing
  `--canlib-sender` and `--canlib-channel`. The gate is that the UCAN receives
  losslessly at ≥90% bus load while both nodes transmit flat out.
- (s/t) paced RX/TX rate sweeps
- (e) a bidirectional soak at 500k (default 300 s)
- (f) a mismatched-bitrate control that must fail, then a check that the
  unacknowledged frames were flushed and are not sent later

`--self-test` checks the loss oracle and the frame-length calculation on their
own.

Why case (i) uses CANlib: through the Linux SocketCAN `kvaser_usb` driver a
Kvaser Leaf Light HS v2 transmits at most about 3,000 frames/s, natively too.
Through CANlib it reaches about 7,140 frames/s, its own ceiling. Neither fills
a 1 Mbit/s bus alone, so case (i) runs both nodes at once.

Receive path: the CAN interrupt drains the 3-deep hardware FIFO into a RAM ring
(128 frames on the F072). Received frames and TX acks wait for space in the USB
response buffer instead of being dropped, and full TX queues backpressure the
USB OUT endpoint. If frames are ever lost, the next frame carries the Kvaser
overrun flag, so the host counts it in `rx_over_errors`.

Stopping the channel, or the driver's flush request, clears the firmware's TX
queue and RX ring. Frames queued while nobody acknowledged them are therefore
never sent later.

Measured against a Kvaser Leaf Light HS v2 (2026-10-07), on the Proxmox host
with both adapters attached natively:

- UCAN receive: with both nodes transmitting flat out, the UCAN received
  20,000/20,000 in order at 100.8% (500k) and 99.3% (1M) measured bus load.
- UCAN transmit: up to 7,701 frames/s at 1M in a burst, and 83.9% bus load
  sustained into a receive-only Leaf. Nothing was lost.
- No frame loss in any case, in either direction.
- Only TX mailbox 0 is used (the vendored HAL keeps frames in order that way),
  so a lone UCAN sender peaks a little below line rate.
- Through a Proxmox VM's USB passthrough, throughput was about halved: about
  1.1 ms per USB round trip. Run load tests natively.

License
----

WTFPL


[CANtact]: <https://linklayer.github.io/cantact/>
[CANable]: <https://canable.io/>
[UCAN]: <https://github.com/FYSETC/UCAN>
