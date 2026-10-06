# fake-bluez

A fake BlueZ 5.72 and bluez-alsa 4.0.0 on a private D-Bus bus. It lets the daemon's Bluetooth
side and the console's Bluetooth tab be driven on a desk machine without a radio, a phone, or any
risk to the desk machine's own Bluetooth: nothing here touches the real system bus.

```sh
make run BT=fake                       # the simulator, with the fake as its "system bus"
tools/fake-bluez/run                   # just the fake, until Ctrl-C
tools/fake-bluez/run CMD ARGS...       # CMD with DBUS_SYSTEM_BUS_ADDRESS on the fake
tools/fake-bluez/run python3 tools/fake-bluez/test_fake.py   # the fake's own self-test
```

`FAKE_BLUEZ_ADAPTERS=2 tools/fake-bluez/run ...` gives it two adapters, as on a desktop with a USB
dongle: the scripted world is in range of hci1, and hci0 (44:A3:BB:36:5E:2E) has nothing in range.
`make pc BT=fake` runs that way, with the daemon on `--bt-adapter hci1`.

`run` starts a `dbus-daemon` on a socket in a private temporary directory, starts
`fake_bluez.py` on it, and stops both when it exits. sd-bus and libdbus both read
`DBUS_SYSTEM_BUS_ADDRESS`, so a client under `run` finds the fake where it expects the real
thing. The address is also written to `$XDG_RUNTIME_DIR/fake-bluez.addr` for `ctl`.

## What it fakes

**org.bluez** — `ObjectManager` on `/`, `AgentManager1` on `/org/bluez`, `Adapter1` on
`/org/bluez/hci0`, `Device1` per device, and `Properties` with `PropertiesChanged` on all of them.
Slow operations answer late, like the real ones (pair ~1 s, connect ~1.5 s, a page timeout 3 s),
and errors carry BlueZ 5.72's own names and messages (checked against its `src/`).

| device | address | pairs by | audio when connected |
|---|---|---|---|
| Galaxy Buds | A0:B1:C2:D3:E4:F5 | paired and trusted at start | playback, 48 kHz |
| JBL Flip 5 | F8:DF:15:0A:11:3C | just works: no question | playback, 48 kHz |
| Pixel 7 | 5C:E9:1E:22:40:01 | `RequestConfirmation` | capture, 44.1 kHz, 2 s after connecting |
| Old Speaker | 00:1A:7D:DA:71:13 | `RequestPinCode`, wants `1234` | playback, 44.1 kHz |
| MX Keys | C7:3B:52:10:9A:1E | `DisplayPasskey` | none; `Connect` is profile-unavailable |
| LE-only beacon | D4:CA:6E:00:00:01 | — | none; hidden by a `bredr` filter |

All but the Buds appear during a scan, staggered over 4 s, with an RSSI that jitters every 2 s.
RSSI is invalidated when the scan stops, and a device found by a scan is forgotten 30 s later
unless it was paired or connected.

**org.bluealsa** — `ObjectManager` and `Manager1` on `/org/bluealsa`, and a `PCM1` object per
audio link: `.../dev_XX/a2dpsrc/sink` for playback to a speaker, `.../dev_XX/a2dpsnk/source` for
capture from a phone. `Codec` is a string, as in 4.0.0. The speaker's `Delay` changes from 150 ms
to 180 ms 5 s after it connects. `Open()` is refused: there is no audio behind it.

## Playing the other side

```sh
tools/fake-bluez/ctl incoming-pair 5C:E9:1E:22:40:01   # Pixel pairs with us: RequestConfirmation
tools/fake-bluez/ctl incoming-pair F8:DF:15:0A:11:3C   # JBL pairs with us: RequestAuthorization
tools/fake-bluez/ctl incoming-pair 00:1A:7D:DA:71:13   # RequestPinCode; only 1234 succeeds
tools/fake-bluez/ctl display C7:3B:52:10:9A:1E         # DisplayPasskey, digits 0..6, then paired
tools/fake-bluez/ctl drop F8:DF:15:0A:11:3C            # link lost; out of range for 15 s
tools/fake-bluez/ctl start-stream 5C:E9:1E:22:40:01    # the phone's capture PCM appears / goes
tools/fake-bluez/ctl stop-stream 5C:E9:1E:22:40:01
tools/fake-bluez/ctl reset                             # back to the initial world
tools/fake-bluez/ctl objects | pcms                    # GetManagedObjects of either service
tools/fake-bluez/ctl busctl ...                        # anything else, on the private bus
```

An incoming pairing goes to the default agent. It is refused without asking if the adapter is
not `Pairable`. After pairing, an untrusted device asks `AuthorizeService` for A2DP before it
connects. `incoming-pair` waits for the outcome (up to the agent's 60 s) and prints it. Every
agent call and its answer is logged on the fake's stdout.

## Where it is not BlueZ

- No audio, and `PCM1.Open()` fails: the ALSA side of the daemon cannot be tested here.
- Real bluez-alsa keeps a phone's capture PCM for the whole A2DP connection; the fake adds it 2 s
  after connecting and `stop-stream` removes it, so that a stream coming and going can be seen.
- Pairing never leaves `Connected` true by itself; BlueZ briefly holds an ACL link after `Pair()`.
- An incoming just-works pairing always asks `RequestAuthorization`; BlueZ skips that for some
  agent capabilities.
- Discovery filters are not merged across clients: the last one set wins.
- `/org/bluez` has no `ProfileManager1` methods, and the adapter has no LE advertising or GATT.
