#!/usr/bin/env python3
"""Self-test for the fake: a scripted BlueZ client, run against it on its private bus.

    tools/fake-bluez/run python3 tools/fake-bluez/test_fake.py

It plays the part soundtesterd will: registers a pairing agent, scans, pairs, connects, watches
bluez-alsa's PCMs appear, answers the questions an incoming pairing asks, forgets a device. Every
call is asynchronous, because the agent has to answer the fake while a Pair() of ours is still
waiting on it — a blocking call would deadlock exactly the way a real client would.
"""

import os
import sys
import time

import dbus
import dbus.bus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROPS = 'org.freedesktop.DBus.Properties'
ADAPTER = 'org.bluez.Adapter1'
DEVICE = 'org.bluez.Device1'
HCI = '/org/bluez/hci0'
PIXEL = '5C:E9:1E:22:40:01'
JBL = 'F8:DF:15:0A:11:3C'
OLD = '00:1A:7D:DA:71:13'
KEYS = 'C7:3B:52:10:9A:1E'
BEACON = 'D4:CA:6E:00:00:01'

failures = 0


def check(cond, what):
    global failures
    print(('ok    ' if cond else 'FAIL  ') + what, flush=True)
    if not cond:
        failures += 1


def dev(addr):
    return HCI + '/dev_' + addr.replace(':', '_')


def pump(seconds):
    end = time.monotonic() + seconds
    ctx = GLib.MainContext.default()
    while time.monotonic() < end:
        ctx.iteration(False) or time.sleep(0.01)


def wait_for(cond, timeout):
    end = time.monotonic() + timeout
    ctx = GLib.MainContext.default()
    while time.monotonic() < end:
        if cond():
            return True
        ctx.iteration(False) or time.sleep(0.01)
    return cond()


class Agent(dbus.service.Object):
    """Answers everything yes, unless told to refuse; remembers what it was asked."""

    def __init__(self, conn, path):
        super().__init__(conn, path)
        self.calls = []
        self.refuse = False

    def note(self, *call):
        self.calls.append(call)
        print('      agent:', *call, flush=True)
        if self.refuse:
            raise dbus.DBusException('refused', name='org.bluez.Error.Rejected')

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Release(self):
        self.note('Release')

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='s')
    def RequestPinCode(self, d):
        self.note('RequestPinCode', str(d))
        return '1234'

    @dbus.service.method('org.bluez.Agent1', in_signature='ouq', out_signature='')
    def DisplayPasskey(self, d, passkey, entered):
        self.note('DisplayPasskey', str(d), int(passkey), int(entered))

    @dbus.service.method('org.bluez.Agent1', in_signature='ou', out_signature='')
    def RequestConfirmation(self, d, passkey):
        self.note('RequestConfirmation', str(d), int(passkey))

    @dbus.service.method('org.bluez.Agent1', in_signature='o', out_signature='')
    def RequestAuthorization(self, d):
        self.note('RequestAuthorization', str(d))

    @dbus.service.method('org.bluez.Agent1', in_signature='os', out_signature='')
    def AuthorizeService(self, d, uuid):
        self.note('AuthorizeService', str(d), str(uuid))

    @dbus.service.method('org.bluez.Agent1', in_signature='', out_signature='')
    def Cancel(self):
        self.note('Cancel')


class Client:
    def __init__(self, conn):
        self.conn = conn

    def call(self, path, iface, method, sig='', args=(), dest='org.bluez', timeout=90):
        box = {}
        self.conn.call_async(dest, path, iface, method, sig, args,
                             lambda *r: box.setdefault('ok', r),
                             lambda e: box.setdefault('err', e), timeout=timeout)
        wait_for(lambda: box, timeout + 1)
        if 'err' in box:
            raise box['err']
        return box.get('ok', ())

    def error_of(self, *a, **k):
        try:
            self.call(*a, **k)
        except dbus.DBusException as e:
            return e.get_dbus_name(), e.get_dbus_message()
        return None, None

    def get(self, path, iface, name, dest='org.bluez'):
        return self.call(path, PROPS, 'Get', 'ss', (iface, name), dest=dest)[0]

    def set(self, path, iface, name, value):
        return self.call(path, PROPS, 'Set', 'ssv', (iface, name, value))

    def objects(self, dest='org.bluez', root='/'):
        return self.call(root, 'org.freedesktop.DBus.ObjectManager', 'GetManagedObjects',
                         dest=dest)[0]

    def pcms(self):
        return self.objects('org.bluealsa', '/org/bluealsa')

    def hook(self, method, *args):
        return self.call('/org/soundtester/fake', 'org.soundtester.Fake1', method,
                         's' * len(args), args)


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    conn = dbus.bus.BusConnection(os.environ['DBUS_SYSTEM_BUS_ADDRESS'])
    c = Client(conn)
    agent = Agent(conn, '/test/agent')

    # -- the agent ------------------------------------------------------------------------------
    c.call('/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent', 'os',
           (dbus.ObjectPath('/test/agent'), 'DisplayYesNo'))
    c.call('/org/bluez', 'org.bluez.AgentManager1', 'RequestDefaultAgent', 'o',
           (dbus.ObjectPath('/test/agent'),))
    name, _ = c.error_of('/org/bluez', 'org.bluez.AgentManager1', 'RegisterAgent', 'os',
                         (dbus.ObjectPath('/test/agent'), 'DisplayYesNo'))
    check(name == 'org.bluez.Error.AlreadyExists', 'a second RegisterAgent is AlreadyExists')

    objs = c.objects()
    check(dev('A0:B1:C2:D3:E4:F5') in objs, 'Galaxy Buds known at start')
    check(objs[dev('A0:B1:C2:D3:E4:F5')][DEVICE]['Paired'], 'and paired')
    check('org.freedesktop.DBus.Properties' in objs[HCI], 'managed objects list Properties, as BlueZ does')

    # -- discovery ------------------------------------------------------------------------------
    c.call(HCI, ADAPTER, 'SetDiscoveryFilter', 'a{sv}', ({'Transport': 'bredr'},))
    c.call(HCI, ADAPTER, 'StartDiscovery')
    name, _ = c.error_of(HCI, ADAPTER, 'StartDiscovery')
    check(name == 'org.bluez.Error.InProgress', 'a second StartDiscovery is InProgress')
    check(wait_for(lambda: c.get(HCI, ADAPTER, 'Discovering'), 1), 'Discovering goes true')
    check(wait_for(lambda: dev(PIXEL) in c.objects(), 5), 'the Pixel appears during the scan')
    check('RSSI' in c.objects()[dev(PIXEL)][DEVICE], 'with an RSSI')
    pump(2.5)
    check(dev(BEACON) not in c.objects(), 'an LE-only beacon stays hidden under a bredr filter')

    # -- outgoing pairing: numeric comparison -------------------------------------------------
    c.call(dev(PIXEL), DEVICE, 'Pair')
    check(any(x[0] == 'RequestConfirmation' and x[1] == dev(PIXEL) for x in agent.calls),
          'pairing the Pixel asks the agent RequestConfirmation')
    check(c.get(dev(PIXEL), DEVICE, 'Paired'), 'Pixel paired')
    name, msg = c.error_of(dev(PIXEL), DEVICE, 'Pair')
    check(name == 'org.bluez.Error.AlreadyExists', 'pairing it again is AlreadyExists')

    c.call(dev(PIXEL), DEVICE, 'Connect')
    check(c.get(dev(PIXEL), DEVICE, 'Connected'), 'Pixel connected')
    src = '/org/bluealsa/hci0/dev_5C_E9_1E_22_40_01/a2dpsnk/source'
    check(wait_for(lambda: src in c.pcms(), 4), 'its capture PCM appears in bluez-alsa')
    if src in c.pcms():
        p = c.pcms()[src]['org.bluealsa.PCM1']
        check(p['Mode'] == 'source' and p['Transport'] == 'A2DP-sink' and p['Sampling'] == 44100
              and p['Codec'] == 'SBC' and p['Device'] == dev(PIXEL), 'with the right properties')

    # -- outgoing pairing: just works ---------------------------------------------------------
    check(wait_for(lambda: dev(JBL) in c.objects(), 3), 'the JBL is there')
    before = len(agent.calls)
    c.call(dev(JBL), DEVICE, 'Pair')
    check(len(agent.calls) == before, 'a just-works pairing we start asks the agent nothing')
    c.call(dev(JBL), DEVICE, 'Connect')
    sink = '/org/bluealsa/hci0/dev_F8_DF_15_0A_11_3C/a2dpsrc/sink'
    check(sink in c.pcms() and c.pcms()[sink]['org.bluealsa.PCM1']['Mode'] == 'sink',
          'the JBL gets a playback PCM')

    c.call(HCI, ADAPTER, 'StopDiscovery')
    pump(0.3)
    name, _ = c.error_of(dev(PIXEL), PROPS, 'Get', 'ss', (DEVICE, 'RSSI'))
    check(name == 'org.freedesktop.DBus.Error.InvalidArgs', 'RSSI goes away when the scan stops')
    name, msg = c.error_of(HCI, ADAPTER, 'StopDiscovery')
    check(name == 'org.bluez.Error.Failed' and msg == 'No discovery started',
          'stopping a scan we do not have is Failed: No discovery started')

    # -- incoming pairing: a legacy PIN, then a service authorisation -------------------------
    r = c.hook('IncomingPair', OLD)[0]
    check(r == 'paired+authorized+connected', 'the old speaker pairs with PIN 1234: ' + r)
    check(any(x[0] == 'RequestPinCode' for x in agent.calls), 'the agent was asked for a PIN')
    check(any(x[0] == 'AuthorizeService' for x in agent.calls), 'then to authorise A2DP')

    # -- incoming pairing, refused ------------------------------------------------------------
    c.call(HCI, ADAPTER, 'RemoveDevice', 'o', (dbus.ObjectPath(dev(PIXEL)),))
    check(dev(PIXEL) not in c.objects(), 'RemoveDevice forgets the Pixel')
    check(src not in c.pcms(), 'and its PCM goes with it')
    agent.refuse = True
    r = c.hook('IncomingPair', PIXEL)[0]
    agent.refuse = False
    check(r == 'pairing failed: org.bluez.Error.AuthenticationRejected',
          'a refused confirmation is AuthenticationRejected: ' + r)

    # -- displayed passkey ----------------------------------------------------------------------
    n = len(agent.calls)
    r = c.hook('Display', KEYS)[0]
    shown = [x for x in agent.calls[n:] if x[0] == 'DisplayPasskey']
    check(r == 'paired' and len(shown) == 7 and shown[-1][3] == 6,
          'DisplayPasskey counts the typed digits up to six')

    # -- the link drops -------------------------------------------------------------------------
    c.hook('DropLink', JBL)
    check(not c.get(dev(JBL), DEVICE, 'Connected') and sink not in c.pcms(),
          'DropLink disconnects the JBL and drops its PCM')
    name, msg = c.error_of(dev(JBL), DEVICE, 'Connect')
    check(name == 'org.bluez.Error.Failed' and msg == 'br-connection-page-timeout',
          'reconnecting out of range is Failed: br-connection-page-timeout')

    # -- discoverable, and its timeout --------------------------------------------------------
    c.set(HCI, ADAPTER, 'DiscoverableTimeout', dbus.UInt32(3))
    c.set(HCI, ADAPTER, 'Discoverable', dbus.Boolean(True))
    check(c.get(HCI, ADAPTER, 'Discoverable'), 'discoverable')
    check(wait_for(lambda: not c.get(HCI, ADAPTER, 'Discoverable'), 5),
          'and not any more once the 3 s timeout runs out')

    # -- power ----------------------------------------------------------------------------------
    c.set(HCI, ADAPTER, 'Powered', dbus.Boolean(False))
    check(not c.get(dev(OLD), DEVICE, 'Connected'), 'powering off disconnects everything')
    name, msg = c.error_of(HCI, PROPS, 'Set', 'ssv', (ADAPTER, 'Discoverable', dbus.Boolean(True)))
    check(name == 'org.bluez.Error.Failed' and msg == 'Not Powered',
          'discoverable while off is Failed: Not Powered')
    name, _ = c.error_of(HCI, PROPS, 'Set', 'ssv', (ADAPTER, 'Address', 'x'))
    check(name == 'org.freedesktop.DBus.Error.PropertyReadOnly', 'Address is read-only')
    c.set(HCI, ADAPTER, 'Powered', dbus.Boolean(True))

    c.hook('Reset')
    check(set(c.objects()) == {'/org/bluez', HCI, dev('A0:B1:C2:D3:E4:F5')} and not c.pcms(),
          'Reset puts the initial world back')

    print('PASS' if failures == 0 else 'FAILED: %d check(s)' % failures, flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
