# fake-bluez

Fake BlueZ 5.72 + bluez-alsa 4.0.0 on a private D-Bus bus, to drive the daemon's and console's
Bluetooth without radio or phone. Never touches the real system bus.

```sh
make run BT=fake                       # simulator, the fake as its "system bus"
tools/fake-bluez/run                   # just the fake, until Ctrl-C
tools/fake-bluez/run CMD ARGS...       # CMD with DBUS_SYSTEM_BUS_ADDRESS on the fake
tools/fake-bluez/run python3 tools/fake-bluez/test_fake.py   # self-test
```

- `run` starts `dbus-daemon` (socket in a private temp dir) + `fake_bluez.py`, stops both on exit.
  sd-bus and libdbus honour `DBUS_SYSTEM_BUS_ADDRESS`; the address also goes to
  `$XDG_RUNTIME_DIR/fake-bluez.addr` for `ctl`.
- `FAKE_BLUEZ_ADAPTERS=2`: two adapters (desktop + dongle); the world is in range of hci1, hci0
  (44:A3:BB:36:5E:2E) sees nothing. `make pc BT=fake` uses it, daemon on `--bt-adapter hci1`.

## What it fakes

**org.bluez**: `ObjectManager` on `/`, `AgentManager1` on `/org/bluez`, `Adapter1` on
`/org/bluez/hci0`, `Device1` per device, `Properties` + `PropertiesChanged` on all. Slow calls
answer late (pair ~1 s, connect ~1.5 s, page timeout 3 s); errors use BlueZ 5.72's names and
messages (from its `src/`).

| device | address | pairs by | audio when connected |
|---|---|---|---|
| Galaxy Buds | A0:B1:C2:D3:E4:F5 | paired and trusted at start | playback, 48 kHz |
| JBL Flip 5 | F8:DF:15:0A:11:3C | just works | playback, 48 kHz |
| Pixel 7 | 5C:E9:1E:22:40:01 | `RequestConfirmation` | capture, 44.1 kHz, 2 s after connecting |
| Old Speaker | 00:1A:7D:DA:71:13 | `RequestPinCode`, wants `1234` | playback, 44.1 kHz |
| MX Keys | C7:3B:52:10:9A:1E | `DisplayPasskey` | none; `Connect` is profile-unavailable |
| LE-only beacon | D4:CA:6E:00:00:01 | — | none; hidden by a `bredr` filter |

Scans find all but the Buds, staggered over 4 s, RSSI jittering every 2 s and invalidated when the
scan stops. Scanned devices are forgotten after 30 s unless paired or connected.

**org.bluealsa**: `ObjectManager`, `Manager1` on `/org/bluealsa`; one `PCM1` per link:
`.../dev_XX/a2dpsrc/sink` (to a speaker), `.../dev_XX/a2dpsnk/source` (from a phone). `Codec` is a
string (as in 4.0.0). Speaker `Delay` goes 150 → 180 ms 5 s after connecting. `Open()` is refused.

## Playing the other side

```sh
tools/fake-bluez/ctl incoming-pair 5C:E9:1E:22:40:01   # Pixel: RequestConfirmation
tools/fake-bluez/ctl incoming-pair F8:DF:15:0A:11:3C   # JBL: RequestAuthorization
tools/fake-bluez/ctl incoming-pair 00:1A:7D:DA:71:13   # RequestPinCode; only 1234 succeeds
tools/fake-bluez/ctl display C7:3B:52:10:9A:1E         # DisplayPasskey, digits 0..6, then paired
tools/fake-bluez/ctl drop F8:DF:15:0A:11:3C            # link lost; out of range 15 s
tools/fake-bluez/ctl start-stream 5C:E9:1E:22:40:01    # phone's capture PCM appears
tools/fake-bluez/ctl stop-stream 5C:E9:1E:22:40:01     # ...and goes
tools/fake-bluez/ctl reset                             # initial world
tools/fake-bluez/ctl objects | pcms                    # GetManagedObjects of either service
tools/fake-bluez/ctl busctl ...                        # anything else on the private bus
```

Incoming pairings go to the default agent, refused unasked if the adapter isn't `Pairable`. An
untrusted device then asks `AuthorizeService` for A2DP before connecting. `incoming-pair` waits for
the outcome (up to the agent's 60 s) and prints it. Agent calls and answers are logged to stdout.

## Where it is not BlueZ

- No audio (`PCM1.Open()` fails): the daemon's ALSA side is untestable here.
- Real bluez-alsa keeps a phone's capture PCM for the whole connection; the fake adds it 2 s in
  and `stop-stream` removes it, to show a stream coming and going.
- `Pair()` never leaves `Connected` true (BlueZ briefly holds an ACL link).
- Incoming just-works always asks `RequestAuthorization`; BlueZ skips it for some agent
  capabilities.
- Discovery filters aren't merged across clients: last wins.
- No `ProfileManager1` methods on `/org/bluez`; no LE advertising or GATT.
