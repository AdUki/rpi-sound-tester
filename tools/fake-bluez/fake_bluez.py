#!/usr/bin/env python3
"""A fake BlueZ 5.72 and bluez-alsa 4.0.0, for a private D-Bus bus.

soundtesterd's Bluetooth side talks to two services: org.bluez (the adapter, the devices, the
pairing agent) and org.bluealsa (the audio links). Neither can be exercised on a laptop without
taking over that laptop's own radio, and pairing cannot be exercised at all without a phone in the
hand. This plays both services on a bus of its own, with a small scripted world of devices, so
the daemon and the console can be driven end to end from a desk.

It is faithful where the daemon could tell the difference: object paths, interface and property
names, D-Bus types, which errors come back and with what name, which calls go to the pairing
agent and with what arguments, and that the slow operations answer late. It is not a radio: no
audio flows, and PCM1.Open() is refused.

Run it through ./run, which starts the bus. Test hooks live on /org/soundtester/fake
(org.soundtester.Fake1); ./ctl wraps them.
"""

import argparse
import random
import sys
import time

import dbus
import dbus.bus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROPS = 'org.freedesktop.DBus.Properties'
OBJMGR = 'org.freedesktop.DBus.ObjectManager'
INTROSPECT = 'org.freedesktop.DBus.Introspectable'
ADAPTER = 'org.bluez.Adapter1'
DEVICE = 'org.bluez.Device1'
AGENT_MGR = 'org.bluez.AgentManager1'
AGENT = 'org.bluez.Agent1'
PCM = 'org.bluealsa.PCM1'
BA_MGR = 'org.bluealsa.Manager1'
FAKE = 'org.soundtester.Fake1'

ADAPTER_PATH = '/org/bluez/hci0'
BA_ROOT = '/org/bluealsa'

UUID_AUDIO_SOURCE = '0000110a-0000-1000-8000-00805f9b34fb'
UUID_AUDIO_SINK = '0000110b-0000-1000-8000-00805f9b34fb'
UUID_AVRCP_TARGET = '0000110c-0000-1000-8000-00805f9b34fb'
UUID_A2DP = '0000110d-0000-1000-8000-00805f9b34fb'
UUID_AVRCP = '0000110e-0000-1000-8000-00805f9b34fb'
UUID_HFP = '0000111e-0000-1000-8000-00805f9b34fb'
UUID_HID = '00001124-0000-1000-8000-00805f9b34fb'
UUID_PNP = '00001200-0000-1000-8000-00805f9b34fb'

# BlueZ's agent timeout (src/agent.c REQUEST_TIMEOUT): a question nobody answers fails after this.
AGENT_TIMEOUT_S = 60
# How long a device found by a scan outlives the scan, as BlueZ's TemporaryTimeout does.
TEMPORARY_TIMEOUT_S = 30
# How long DropLink keeps a device out of reach before it can be connected again.
OUT_OF_RANGE_S = 15


def log(*args):
    print(time.strftime('%H:%M:%S'), *args, flush=True)


def err(name, msg):
    """An error as BlueZ names it: org.bluez.Error.<name>, with BlueZ's own message."""
    return dbus.DBusException(msg, name='org.bluez.Error.' + name)


def dev_path(address):
    return ADAPTER_PATH + '/dev_' + address.replace(':', '_')


def strv(items):
    return dbus.Array(items, signature='s')


def empty():
    return dbus.Dictionary({}, signature='sv')


def signature_of(v):
    for t, sig in ((dbus.Boolean, 'b'), (dbus.Byte, 'y'), (dbus.Int16, 'n'), (dbus.UInt16, 'q'),
                   (dbus.UInt32, 'u'), (dbus.ObjectPath, 'o'), (dbus.String, 's')):
        if isinstance(v, t):
            return sig
    if isinstance(v, dbus.Array):
        return 'a' + (v.signature or 's')
    return 'v'


# ---- the world ----------------------------------------------------------------------------------
#
# What a scan can find. `pairing` says how the device pairs: 'justworks' (no question asked when
# we start it; RequestAuthorization when it starts it), 'confirm' (numeric comparison: both sides
# show a passkey), 'pin' (a legacy device: RequestPinCode), 'display' (we show a passkey the other
# side types: DisplayPasskey). `audio` is the bluez-alsa PCM a connection opens: 'playback' to a
# speaker, 'capture' from a phone.

WORLD = [
    dict(address='D4:CA:6E:00:00:01', name='LE-only beacon', address_type='random', icon=None,
         cls=None, uuids=[], pairing='justworks', audio=None, rssi=-88, appears=0.5,
         le_only=True),
    dict(address='F8:DF:15:0A:11:3C', name='JBL Flip 5', icon='audio-card', cls=0x240414,
         uuids=[UUID_AUDIO_SINK, UUID_AVRCP_TARGET, UUID_AVRCP, UUID_HFP, UUID_PNP],
         pairing='justworks', audio='playback', rate=48000, rssi=-58, appears=1.0),
    dict(address='5C:E9:1E:22:40:01', name='Pixel 7', icon='phone', cls=0x5a020c,
         uuids=[UUID_AUDIO_SOURCE, UUID_AVRCP_TARGET, UUID_AVRCP, UUID_PNP],
         pairing='confirm', audio='capture', rate=44100, rssi=-45, appears=2.0),
    dict(address='C7:3B:52:10:9A:1E', name='MX Keys', icon='input-keyboard', cls=0x002540,
         uuids=[UUID_HID, UUID_PNP], pairing='display', audio=None, rssi=-70, appears=3.0),
    dict(address='00:1A:7D:DA:71:13', name='Old Speaker', icon='audio-card', cls=0x240414,
         uuids=[UUID_AUDIO_SINK], pairing='pin', pin='1234', legacy=True, audio='playback',
         rate=44100, rssi=-77, appears=4.0),
    dict(address='A0:B1:C2:D3:E4:F5', name='Galaxy Buds', icon='audio-headphones', cls=0x240404,
         uuids=[UUID_AUDIO_SINK, UUID_AVRCP_TARGET, UUID_AVRCP, UUID_HFP], pairing='confirm',
         audio='playback', rate=48000, rssi=-66, appears=1.5, known=True),
]


def world_entry(address):
    for w in WORLD:
        if w['address'].upper() == address.upper():
            return w
    return None


# ---- property plumbing --------------------------------------------------------------------------


def player_text(props):
    """A player's PropertiesChanged or RegisterPlayer dict, as one line."""
    md = props.get('Metadata', {})
    artist = ', '.join(str(a) for a in md.get('xesam:artist', [])) or '-'
    return '%s "%s" by %s (%s)' % (props.get('PlaybackStatus', '-'), md.get('xesam:title', '-'),
                                   artist, md.get('xesam:album', ''))

class PropObject(dbus.service.Object):
    """An exported object with BlueZ-style properties: GetAll/Get/Set and PropertiesChanged.

    `writable` maps "iface.prop" to a setter that may raise a D-Bus error, which is how a
    read-only property, a bad value or a refused change come back the way BlueZ sends them."""

    def __init__(self, svc, path, ifaces, writable=None, extra_ifaces=()):
        super().__init__(svc.conn, path)
        self.svc = svc
        self.path = path
        self.ifaces = ifaces          # iface -> {name: dbus value}
        self.writable = writable or {}
        # BlueZ lists Introspectable and Properties with empty dicts in GetManagedObjects; GDBus
        # (bluez-alsa) does not. Mirror each, so a client that trips on either is caught here.
        self.extra_ifaces = extra_ifaces

    def managed(self):
        d = dbus.Dictionary({}, signature='sa{sv}')
        for x in self.extra_ifaces:
            d[x] = empty()
        for iface, props in self.ifaces.items():
            d[iface] = dbus.Dictionary(props, signature='sv')
        return d

    def update(self, iface, **changes):
        """Sets properties and announces the ones that actually changed."""
        props = self.ifaces[iface]
        changed = {}
        for k, v in changes.items():
            if k not in props or props[k] != v or type(props[k]) is not type(v):
                props[k] = v
                changed[k] = v
        if changed:
            self.PropertiesChanged(iface, dbus.Dictionary(changed, signature='sv'), strv([]))

    def invalidate(self, iface, *names):
        props = self.ifaces[iface]
        gone = [n for n in names if n in props]
        for n in gone:
            del props[n]
        if gone:
            self.PropertiesChanged(iface, empty(), strv(gone))

    # dbus-python introspects only methods and signals; add the properties, so that busctl
    # introspect shows what GetAll would return.
    @dbus.service.method(INTROSPECT, in_signature='', out_signature='s',
                         path_keyword='object_path', connection_keyword='connection')
    def Introspect(self, object_path, connection):
        xml = dbus.service.Object.Introspect(self, object_path, connection)
        for iface, props in self.ifaces.items():
            rows = ''.join('<property name="%s" type="%s" access="%s"/>' % (
                k, signature_of(v), 'readwrite' if (iface + '.' + k) in self.writable else 'read')
                for k, v in props.items())
            tag = '<interface name="%s">' % iface
            if tag in xml:
                xml = xml.replace(tag, tag + rows, 1)
            else:
                xml = xml.replace('</node>', tag + rows + '</interface></node>', 1)
        return xml

    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, iface, name):
        if iface not in self.ifaces:
            raise dbus.DBusException('No such interface ' + iface,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        if name not in self.ifaces[iface]:
            raise dbus.DBusException('No such property ' + name,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        return self.ifaces[iface][name]

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        if iface not in self.ifaces:
            raise dbus.DBusException('No such interface ' + iface,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        return dbus.Dictionary(self.ifaces[iface], signature='sv')

    @dbus.service.method(PROPS, in_signature='ssv', out_signature='')
    def Set(self, iface, name, value):
        if iface not in self.ifaces or name not in self.ifaces[iface]:
            raise dbus.DBusException('No such property ' + name,
                                     name='org.freedesktop.DBus.Error.InvalidArgs')
        setter = self.writable.get(iface + '.' + name)
        if setter is None:
            raise dbus.DBusException("Property '%s' is not writable" % name,
                                     name='org.freedesktop.DBus.Error.PropertyReadOnly')
        log('Set %s %s.%s = %r' % (self.path, iface, name, value))
        setter(value)

    @dbus.service.signal(PROPS, signature='sa{sv}as')
    def PropertiesChanged(self, iface, changed, invalidated):
        pass


class ObjectManager(dbus.service.Object):
    """GetManagedObjects over a set of PropObjects, with the two signals."""

    def __init__(self, svc, path):
        super().__init__(svc.conn, path)
        self.objects = {}   # path -> PropObject

    def add(self, obj):
        self.objects[obj.path] = obj
        self.InterfacesAdded(dbus.ObjectPath(obj.path), obj.managed())

    def remove(self, obj):
        if self.objects.pop(obj.path, None) is None:
            return
        names = list(obj.extra_ifaces) + list(obj.ifaces.keys())
        self.InterfacesRemoved(dbus.ObjectPath(obj.path), strv(names))
        obj.remove_from_connection()

    @dbus.service.method(OBJMGR, in_signature='', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        out = dbus.Dictionary({}, signature='oa{sa{sv}}')
        for path, obj in self.objects.items():
            out[dbus.ObjectPath(path)] = obj.managed()
        return out

    @dbus.service.signal(OBJMGR, signature='oa{sa{sv}}')
    def InterfacesAdded(self, path, ifaces):
        pass

    @dbus.service.signal(OBJMGR, signature='oas')
    def InterfacesRemoved(self, path, ifaces):
        pass


# ---- org.bluez ----------------------------------------------------------------------------------

class AgentManager(PropObject):
    def __init__(self, svc):
        super().__init__(svc, '/org/bluez', {AGENT_MGR: {}, 'org.bluez.ProfileManager1': {}},
                         extra_ifaces=(INTROSPECT,))

    @dbus.service.method(AGENT_MGR, in_signature='os', out_signature='', sender_keyword='sender')
    def RegisterAgent(self, path, capability, sender=None):
        caps = ('', 'DisplayOnly', 'DisplayYesNo', 'KeyboardOnly', 'NoInputNoOutput',
                'KeyboardDisplay')
        if capability not in caps:
            raise err('InvalidArguments', 'Invalid arguments in method call')
        if sender in self.svc.agents:
            raise err('AlreadyExists', 'Already Exists')
        self.svc.agents[sender] = (str(path), capability or 'KeyboardDisplay')
        log('agent registered: %s %s (%s)' % (sender, path, capability))

    @dbus.service.method(AGENT_MGR, in_signature='o', out_signature='', sender_keyword='sender')
    def UnregisterAgent(self, path, sender=None):
        a = self.svc.agents.get(sender)
        if not a or a[0] != str(path):
            raise err('DoesNotExist', 'Does Not Exist')
        self.svc.drop_agent(sender)

    @dbus.service.method(AGENT_MGR, in_signature='o', out_signature='', sender_keyword='sender')
    def RequestDefaultAgent(self, path, sender=None):
        a = self.svc.agents.get(sender)
        if not a or a[0] != str(path):
            raise err('DoesNotExist', 'Does Not Exist')
        self.svc.default_agent = sender
        log('default agent: %s %s' % (sender, path))


class Adapter(PropObject):
    def __init__(self, svc):
        p = {
            'Address': dbus.String('B8:27:EB:50:7B:22'),
            'AddressType': dbus.String('public'),
            'Name': dbus.String('BlueZ 5.72'),
            'Alias': dbus.String('BlueZ 5.72'),
            'Class': dbus.UInt32(0x000000),
            'Powered': dbus.Boolean(True),
            'PowerState': dbus.String('on'),
            'Discoverable': dbus.Boolean(False),
            'DiscoverableTimeout': dbus.UInt32(180),
            'Pairable': dbus.Boolean(True),
            'PairableTimeout': dbus.UInt32(0),
            'Discovering': dbus.Boolean(False),
            'UUIDs': strv([UUID_AUDIO_SOURCE, UUID_AUDIO_SINK, UUID_AVRCP_TARGET, UUID_AVRCP]),
            'Modalias': dbus.String('usb:v1D6Bp0246d0548'),
            'Roles': strv(['central', 'peripheral']),
        }
        w = {
            ADAPTER + '.Alias': self.set_alias,
            ADAPTER + '.Powered': self.set_powered,
            ADAPTER + '.Discoverable': self.set_discoverable,
            ADAPTER + '.DiscoverableTimeout': self.set_discoverable_timeout,
            ADAPTER + '.Pairable': self.set_pairable,
            ADAPTER + '.PairableTimeout': self.set_pairable_timeout,
        }
        super().__init__(svc, ADAPTER_PATH,
                         {ADAPTER: p, 'org.bluez.Media1': {
                             'SupportedUUIDs': strv([UUID_AUDIO_SOURCE, UUID_AUDIO_SINK])},
                          'org.bluez.GattManager1': {}},
                         writable=w, extra_ifaces=(INTROSPECT, PROPS))
        self.disc_timer = None
        self.pair_timer = None
        self.discovery = {}     # sender -> True while that client scans
        self.filters = {}       # sender -> Transport
        self.scan_timers = []
        self.players = set()    # (sender, path) registered with Media1

    @property
    def p(self):
        return self.ifaces[ADAPTER]

    # -- writable properties --

    def set_alias(self, v):
        # An empty alias puts the system name back, as in BlueZ.
        self.update(ADAPTER, Alias=dbus.String(str(v) or str(self.p['Name'])))

    def set_powered(self, v):
        on = bool(v)
        if on == bool(self.p['Powered']):
            return
        if not on:
            # Everything that needs the radio stops with it.
            for sender in list(self.discovery):
                self.discovery.pop(sender)
            self.sync_discovering()
            self.update(ADAPTER, Discoverable=dbus.Boolean(False))
            self.cancel_timer('disc_timer')
            for d in list(self.svc.devices.values()):
                d.link_lost('adapter powered off')
        self.update(ADAPTER, Powered=dbus.Boolean(on),
                    PowerState=dbus.String('on' if on else 'off'))

    def set_discoverable(self, v):
        on = bool(v)
        if on and not self.p['Powered']:
            raise err('Failed', 'Not Powered')
        self.update(ADAPTER, Discoverable=dbus.Boolean(on))
        self.arm_discoverable()

    def set_discoverable_timeout(self, v):
        self.update(ADAPTER, DiscoverableTimeout=dbus.UInt32(int(v)))
        self.arm_discoverable()

    def set_pairable(self, v):
        self.update(ADAPTER, Pairable=dbus.Boolean(bool(v)))
        self.arm_pairable()

    def set_pairable_timeout(self, v):
        self.update(ADAPTER, PairableTimeout=dbus.UInt32(int(v)))
        self.arm_pairable()

    # -- timeouts --

    def cancel_timer(self, attr):
        t = getattr(self, attr)
        if t is not None:
            GLib.source_remove(t)
            setattr(self, attr, None)

    def arm_discoverable(self):
        # Restarted by any change, the way the kernel restarts its own countdown.
        self.cancel_timer('disc_timer')
        secs = int(self.p['DiscoverableTimeout'])
        if self.p['Discoverable'] and secs > 0:
            def expire():
                self.disc_timer = None
                log('discoverable timeout expired')
                self.update(ADAPTER, Discoverable=dbus.Boolean(False))
                return False
            self.disc_timer = GLib.timeout_add_seconds(secs, expire)

    def arm_pairable(self):
        self.cancel_timer('pair_timer')
        secs = int(self.p['PairableTimeout'])
        if self.p['Pairable'] and secs > 0:
            def expire():
                self.pair_timer = None
                self.update(ADAPTER, Pairable=dbus.Boolean(False))
                return False
            self.pair_timer = GLib.timeout_add_seconds(secs, expire)

    # -- discovery --

    def transport(self):
        """The filter in force. BlueZ merges every client's filter; the last one set will do."""
        t = 'auto'
        for v in self.filters.values():
            t = v
        return t

    def sync_discovering(self):
        on = bool(self.discovery)
        if on == bool(self.p['Discovering']):
            return
        self.update(ADAPTER, Discovering=dbus.Boolean(on))
        if on:
            self.start_world()
        else:
            for t in self.scan_timers:
                GLib.source_remove(t)
            self.scan_timers = []
            for d in list(self.svc.devices.values()):
                d.discovery_stopped()

    def start_world(self):
        t = self.transport()
        for w in WORLD:
            if w.get('le_only') and t == 'bredr':
                continue
            if not w.get('le_only') and t == 'le':
                continue

            tid = []

            def appear(w=w, tid=tid):
                # Fired, so no longer ours to cancel when the scan stops.
                self.scan_timers.remove(tid[0])
                d = self.svc.devices.get(w['address'])
                if d is None:
                    d = self.svc.add_device(w)
                d.seen(w['rssi'])
                return False
            tid.append(GLib.timeout_add(int(w['appears'] * 1000), appear))
            self.scan_timers.append(tid[0])

        def jitter():
            if not self.p['Discovering']:
                return False
            for d in self.svc.devices.values():
                if d.in_range and 'RSSI' in d.p:
                    d.update(DEVICE, RSSI=dbus.Int16(d.w['rssi'] + random.randint(-4, 4)))
            return True
        self.scan_timers.append(GLib.timeout_add(2000, jitter))

    # Media1: the media player a client offers to AVRCP controllers (soundtesterd's own). Logged,
    # and its track again whenever it changes, which is what a speaker would be told.
    @dbus.service.method('org.bluez.Media1', in_signature='oa{sv}', out_signature='',
                         sender_keyword='sender')
    def RegisterPlayer(self, path, props, sender=None):
        if (sender, path) in self.players:
            raise err('AlreadyExists', 'Already Exists')
        self.players.add((sender, path))
        log('RegisterPlayer %s by %s: %s' % (path, sender, player_text(props)))

        def changed(_iface, props, _invalidated):
            log('player %s: %s' % (path, player_text(props)))
        self.svc.conn.add_signal_receiver(changed, 'PropertiesChanged', PROPS, sender, str(path))

    @dbus.service.method('org.bluez.Media1', in_signature='o', out_signature='',
                         sender_keyword='sender')
    def UnregisterPlayer(self, path, sender=None):
        self.players.discard((sender, path))

    @dbus.service.method(ADAPTER, in_signature='', out_signature='', sender_keyword='sender')
    def StartDiscovery(self, sender=None):
        if not self.p['Powered']:
            raise err('NotReady', 'Resource Not Ready')
        if sender in self.discovery:
            raise err('InProgress', 'Operation already in progress')
        log('StartDiscovery by %s (transport %s)' % (sender, self.filters.get(sender, 'auto')))
        self.discovery[sender] = True
        self.sync_discovering()

    @dbus.service.method(ADAPTER, in_signature='', out_signature='', sender_keyword='sender')
    def StopDiscovery(self, sender=None):
        if not self.p['Powered']:
            raise err('NotReady', 'Resource Not Ready')
        if sender not in self.discovery:
            raise err('Failed', 'No discovery started')
        log('StopDiscovery by %s' % sender)
        del self.discovery[sender]
        self.sync_discovering()

    @dbus.service.method(ADAPTER, in_signature='a{sv}', out_signature='', sender_keyword='sender')
    def SetDiscoveryFilter(self, f, sender=None):
        t = str(f.get('Transport', 'auto'))
        if t not in ('auto', 'bredr', 'le'):
            raise err('InvalidArguments', 'Invalid arguments in method call')
        for k in f.keys():
            if k not in ('UUIDs', 'RSSI', 'Pathloss', 'Transport', 'DuplicateData',
                         'Discoverable', 'Pattern'):
                raise err('InvalidArguments', 'Invalid arguments in method call')
        if f:
            self.filters[sender] = t
        else:
            self.filters.pop(sender, None)   # an empty dict clears the client's filter
        log('SetDiscoveryFilter by %s: %s' % (sender, dict(f)))

    @dbus.service.method(ADAPTER, in_signature='', out_signature='as')
    def GetDiscoveryFilters(self):
        return strv(['UUIDs', 'RSSI', 'Pathloss', 'Transport', 'DuplicateData', 'Discoverable',
                     'Pattern'])

    @dbus.service.method(ADAPTER, in_signature='o', out_signature='')
    def RemoveDevice(self, path):
        d = next((x for x in self.svc.devices.values() if x.path == str(path)), None)
        if d is None:
            raise err('DoesNotExist', 'Does Not Exist')
        log('RemoveDevice %s' % d.address)
        self.svc.remove_device(d)

    def client_gone(self, sender):
        self.filters.pop(sender, None)
        if self.discovery.pop(sender, None):
            self.sync_discovering()


class Device(PropObject):
    def __init__(self, svc, w):
        self.w = w
        self.address = w['address']
        p = {
            'Address': dbus.String(w['address']),
            'AddressType': dbus.String(w.get('address_type', 'public')),
            'Name': dbus.String(w['name']),
            'Alias': dbus.String(w['name']),
            'Paired': dbus.Boolean(bool(w.get('known'))),
            'Bonded': dbus.Boolean(bool(w.get('known'))),
            'Trusted': dbus.Boolean(bool(w.get('known'))),
            'Blocked': dbus.Boolean(False),
            'LegacyPairing': dbus.Boolean(bool(w.get('legacy'))),
            'Connected': dbus.Boolean(False),
            'ServicesResolved': dbus.Boolean(False),
            'UUIDs': strv(w['uuids']),
            'Adapter': dbus.ObjectPath(ADAPTER_PATH),
            'WakeAllowed': dbus.Boolean(False),
        }
        if w.get('icon'):
            p['Icon'] = dbus.String(w['icon'])
        if w.get('cls') is not None:
            p['Class'] = dbus.UInt32(w['cls'])
        wr = {
            DEVICE + '.Alias': self.set_alias,
            DEVICE + '.Trusted': self.set_trusted,
            DEVICE + '.Blocked': self.set_blocked,
            DEVICE + '.WakeAllowed': lambda v: self.update(DEVICE, WakeAllowed=dbus.Boolean(v)),
        }
        super().__init__(svc, dev_path(w['address']), {DEVICE: p}, writable=wr,
                         extra_ifaces=(INTROSPECT, PROPS))
        self.in_range = True
        self.range_timer = None
        self.forget_timer = None
        self.busy = None          # 'pair' | 'connect' while one of them runs
        self.attempt = 0          # bumped to orphan a late agent reply after a cancel
        self.pending_pair = None  # (ok, err) of an outstanding Pair(), for CancelPairing
        self.agent_waiting = None  # the agent (sender, path) we are waiting on, if any

    @property
    def p(self):
        return self.ifaces[DEVICE]

    def set_alias(self, v):
        self.update(DEVICE, Alias=dbus.String(str(v) or str(self.p['Name'])))

    def set_trusted(self, v):
        self.update(DEVICE, Trusted=dbus.Boolean(bool(v)))

    def set_blocked(self, v):
        self.update(DEVICE, Blocked=dbus.Boolean(bool(v)))
        if v:
            self.link_lost('blocked')

    # -- presence --

    def seen(self, rssi):
        self.cancel_forget()
        if self.in_range:
            self.update(DEVICE, RSSI=dbus.Int16(rssi))

    def discovery_stopped(self):
        # RSSI is only meaningful while scanning: BlueZ invalidates it when the scan ends, and
        # forgets a device the scan found unless it was paired or connected meanwhile.
        self.invalidate(DEVICE, 'RSSI')
        if not self.p['Paired'] and not self.p['Connected'] and not self.p['Trusted']:
            def forget():
                self.forget_timer = None
                if not self.p['Paired'] and not self.p['Connected'] and self.busy is None:
                    log('temporary device %s expired' % self.address)
                    self.svc.remove_device(self)
                return False
            self.cancel_forget()
            self.forget_timer = GLib.timeout_add_seconds(TEMPORARY_TIMEOUT_S, forget)

    def cancel_forget(self):
        if self.forget_timer is not None:
            GLib.source_remove(self.forget_timer)
            self.forget_timer = None

    # -- the link --

    def connected(self):
        self.update(DEVICE, Connected=dbus.Boolean(True), ServicesResolved=dbus.Boolean(True))
        self.svc.seq += 1
        a = self.w.get('audio')
        if a == 'playback':
            self.svc.bluealsa.add_pcm(self, 'playback')
        elif a == 'capture':
            # A phone opens its stream when it starts to play, a moment after the link is up.
            GLib.timeout_add(2000, lambda: (self.p['Connected'] and
                                            self.svc.bluealsa.add_pcm(self, 'capture'), False)[1])

    def link_lost(self, why):
        if self.p['Connected']:
            log('%s disconnected (%s)' % (self.address, why))
        self.svc.bluealsa.remove_pcms(self)
        self.update(DEVICE, Connected=dbus.Boolean(False), ServicesResolved=dbus.Boolean(False))

    def go_out_of_range(self):
        self.in_range = False
        self.invalidate(DEVICE, 'RSSI')
        if self.range_timer is not None:
            GLib.source_remove(self.range_timer)

        def back():
            self.range_timer = None
            self.in_range = True
            log('%s back in range' % self.address)
            return False
        self.range_timer = GLib.timeout_add_seconds(OUT_OF_RANGE_S, back)

    # -- pairing, shared by Pair() and an incoming pairing --

    def run_pairing(self, agent, incoming, done):
        """Asks `agent` whatever this device's pairing asks, then calls done(error_or_None)."""
        self.attempt += 1
        attempt = self.attempt
        kind = self.w['pairing']
        dev = dbus.ObjectPath(self.path)

        def finish(e):
            if attempt != self.attempt:
                return   # cancelled meanwhile; the canceller has answered already
            self.agent_waiting = None
            if e is None:
                self.update(DEVICE, Paired=dbus.Boolean(True), Bonded=dbus.Boolean(True))
            done(e)

        def agent_error(e):
            name = e.get_dbus_name() if hasattr(e, 'get_dbus_name') else ''
            log('  agent answered with error %s: %s' % (name, e))
            if name == 'org.bluez.Error.Rejected':
                return err('AuthenticationRejected', 'Authentication Rejected')
            if name == 'org.bluez.Error.Canceled':
                return err('AuthenticationCanceled', 'Authentication Canceled')
            if name == 'org.freedesktop.DBus.Error.NoReply':
                return err('AuthenticationTimeout', 'Authentication Timeout')
            return err('AuthenticationFailed', 'Authentication Failed')

        if kind == 'justworks' and not incoming:
            # BlueZ accepts a just-works pairing it started itself without asking anyone.
            GLib.timeout_add(1000, lambda: (finish(None), False)[1])
            return

        if agent is None:
            log('  no agent to ask: pairing %s fails' % self.address)
            GLib.timeout_add(500, lambda: (finish(err('AuthenticationFailed',
                                                      'Authentication Failed')), False)[1])
            return

        if kind == 'display':
            self.display_sequence(agent, lambda e: finish(e))
            return

        if kind == 'confirm':
            passkey = random.randint(0, 999999)
            method, sig, args = 'RequestConfirmation', 'ou', (dev, dbus.UInt32(passkey))
        elif kind == 'justworks':
            method, sig, args = 'RequestAuthorization', 'o', (dev,)
        else:  # pin
            method, sig, args = 'RequestPinCode', 'o', (dev,)

        def reply(*out):
            log('  agent %s -> %r' % (method, out))
            if method == 'RequestPinCode' and str(out[0] if out else '') != self.w.get('pin'):
                finish(err('AuthenticationFailed', 'Authentication Failed'))
            else:
                finish(None)

        self.agent_waiting = agent
        self.svc.call_agent(agent, method, sig, args, reply,
                            lambda e: finish(agent_error(e)))

    def display_sequence(self, agent, done):
        """DisplayPasskey with the count of typed digits climbing, as a keyboard pairing does."""
        passkey = dbus.UInt32(random.randint(0, 999999))
        dev = dbus.ObjectPath(self.path)
        step = [0]
        attempt = self.attempt

        def tick():
            if attempt != self.attempt:
                return False
            if step[0] > 6:
                done(None)
                return False
            self.svc.call_agent(agent, 'DisplayPasskey', 'ouq',
                                (dev, passkey, dbus.UInt16(step[0])),
                                lambda *a: None, lambda e: log('  DisplayPasskey error: %s' % e))
            step[0] += 1
            return True
        GLib.timeout_add(500, tick)

    # -- Device1 methods --

    @dbus.service.method(DEVICE, in_signature='', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def Pair(self, sender=None, ok=None, fail=None):
        log('Pair %s by %s' % (self.address, sender))
        if self.busy == 'pair':
            return fail(err('InProgress', 'In Progress'))
        if self.p['Paired']:
            return fail(err('AlreadyExists', 'Already Exists'))
        if not self.svc.adapter.p['Powered']:
            return fail(err('NotReady', 'Resource Not Ready'))
        if not self.in_range:
            return GLib.timeout_add(3000, lambda: (fail(err('ConnectionAttemptFailed',
                                                            'Page Timeout')), False)[1])
        self.busy = 'pair'
        self.pending_pair = (ok, fail)

        def done(e):
            self.busy = None
            self.pending_pair = None
            log('Pair %s: %s' % (self.address, 'ok' if e is None else e.get_dbus_name()))
            if e is None:
                ok()
            else:
                fail(e)
        # The caller's own agent if it has one, else the default — as BlueZ's agent_get(sender).
        self.run_pairing(self.svc.agent_for(sender), False, done)

    @dbus.service.method(DEVICE, in_signature='', out_signature='')
    def CancelPairing(self):
        if self.pending_pair is None:
            raise err('DoesNotExist', 'Does Not Exist')
        log('CancelPairing %s' % self.address)
        ok, fail = self.pending_pair
        self.pending_pair = None
        self.busy = None
        self.attempt += 1       # orphan the outstanding agent call
        if self.agent_waiting is not None:
            self.svc.call_agent(self.agent_waiting, 'Cancel', '', (), lambda *a: None,
                                lambda e: None)
            self.agent_waiting = None
        fail(err('AuthenticationCanceled', 'Authentication Canceled'))

    @dbus.service.method(DEVICE, in_signature='', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def Connect(self, sender=None, ok=None, fail=None):
        log('Connect %s by %s' % (self.address, sender))
        if self.busy is not None:
            return fail(err('InProgress', 'br-connection-busy'))
        if not self.svc.adapter.p['Powered']:
            return fail(err('NotReady', 'br-connection-adapter-not-powered'))
        if self.p['Connected']:
            return ok()   # BlueZ answers an already-connected device with success
        if not self.w.get('audio'):
            return GLib.timeout_add(800, lambda: (fail(err(
                'NotAvailable', 'br-connection-profile-unavailable')), False)[1])
        if not self.in_range:
            return GLib.timeout_add(3000, lambda: (fail(err(
                'Failed', 'br-connection-page-timeout')), False)[1])
        self.busy = 'connect'

        def up():
            self.busy = None
            self.cancel_forget()
            if not self.p['Paired']:
                # A2DP needs an authenticated link, so a just-works device bonds on the way up.
                self.update(DEVICE, Paired=dbus.Boolean(True), Bonded=dbus.Boolean(True))
            self.connected()
            log('Connect %s: ok' % self.address)
            ok()
            return False

        if self.p['Paired'] or self.w['pairing'] == 'justworks':
            GLib.timeout_add(1500, up)
            return

        # An unpaired device that needs a question pairs as part of the connection, as BlueZ
        # does when a profile needs security.
        def paired(e):
            self.busy = None
            if e is not None:
                fail(e)
            else:
                self.busy = 'connect'
                GLib.timeout_add(1000, up)
        self.run_pairing(self.svc.agent_for(sender), False, paired)

    @dbus.service.method(DEVICE, in_signature='', out_signature='', async_callbacks=('ok', 'fail'))
    def Disconnect(self, ok=None, fail=None):
        log('Disconnect %s' % self.address)
        if not self.p['Connected']:
            return ok()

        def down():
            self.link_lost('Disconnect()')
            ok()
            return False
        GLib.timeout_add(500, down)

    @dbus.service.method(DEVICE, in_signature='s', out_signature='', sender_keyword='sender',
                         async_callbacks=('ok', 'fail'))
    def ConnectProfile(self, uuid, sender=None, ok=None, fail=None):
        if str(uuid).lower() not in [u.lower() for u in self.w['uuids']] + [UUID_A2DP]:
            return fail(err('InvalidArguments', 'Invalid arguments in method call'))
        self.Connect(sender=sender, ok=ok, fail=fail)

    @dbus.service.method(DEVICE, in_signature='s', out_signature='', async_callbacks=('ok', 'fail'))
    def DisconnectProfile(self, uuid, ok=None, fail=None):
        self.Disconnect(ok=ok, fail=fail)


# ---- org.bluealsa -------------------------------------------------------------------------------

class Pcm(PropObject):
    def __init__(self, svc, dev, direction):
        playback = direction == 'playback'
        suffix = '/a2dpsrc/sink' if playback else '/a2dpsnk/source'
        path = BA_ROOT + '/hci0/dev_' + dev.address.replace(':', '_') + suffix
        p = {
            'Device': dbus.ObjectPath(dev.path),
            'Sequence': dbus.UInt32(svc.seq),
            'Transport': dbus.String('A2DP-source' if playback else 'A2DP-sink'),
            'Mode': dbus.String('sink' if playback else 'source'),
            'Format': dbus.UInt16(0x8210),      # signed, little-endian, 2 bytes, 16 bits
            'Channels': dbus.Byte(2),
            'Sampling': dbus.UInt32(dev.w.get('rate', 48000)),
            'Codec': dbus.String('SBC'),        # a string in 4.0.0 (ba_variant_new_pcm_codec)
            'Delay': dbus.UInt16(1500 if playback else 400),
            'SoftVolume': dbus.Boolean(not playback),
            'Volume': dbus.UInt16(0x7f7f),
        }
        w = {PCM + '.SoftVolume': lambda v: self.update(PCM, SoftVolume=dbus.Boolean(v)),
             PCM + '.Volume': lambda v: self.update(PCM, Volume=dbus.UInt16(v))}
        super().__init__(svc, path, {PCM: p}, writable=w)
        self.direction = direction

    @dbus.service.method(PCM, in_signature='', out_signature='hh')
    def Open(self):
        # There is no audio to hand out: the fake has no transport behind it.
        raise dbus.DBusException('Not supported by the fake',
                                 name='org.freedesktop.DBus.Error.NotSupported')

    @dbus.service.method(PCM, in_signature='', out_signature='a{sa{sv}}')
    def GetCodecs(self):
        return dbus.Dictionary({'SBC': empty()}, signature='sa{sv}')

    @dbus.service.method(PCM, in_signature='sa{sv}', out_signature='')
    def SelectCodec(self, codec, props):
        if str(codec).upper() != 'SBC':
            raise dbus.DBusException('Codec not supported',
                                     name='org.freedesktop.DBus.Error.NotSupported')


class BluealsaManager(ObjectManager):
    """/org/bluealsa: both the ObjectManager and the (deprecated) Manager1 live here in 4.0.0."""

    def __init__(self, svc):
        super().__init__(svc, BA_ROOT)
        self.svc = svc
        self.pcms = {}   # path -> Pcm

    def add_pcm(self, dev, direction):
        pcm = Pcm(self.svc, dev, direction)
        if pcm.path in self.pcms:
            pcm.remove_from_connection()
            return
        self.pcms[pcm.path] = pcm
        self.add(pcm)
        self.PCMAdded(dbus.ObjectPath(pcm.path), dbus.Dictionary(pcm.ifaces[PCM], signature='sv'))
        log('bluealsa: %s PCM for %s at %s' % (direction, dev.address, pcm.path))
        if direction == 'playback':
            # A speaker revises its reported delay once it has settled; let the console see one.
            def revise():
                if pcm.path in self.pcms:
                    pcm.update(PCM, Delay=dbus.UInt16(1800))
                return False
            GLib.timeout_add_seconds(5, revise)

    def remove_pcms(self, dev, direction=None):
        for path, pcm in list(self.pcms.items()):
            if pcm.ifaces[PCM]['Device'] == dev.path and direction in (None, pcm.direction):
                del self.pcms[path]
                self.remove(pcm)
                self.PCMRemoved(dbus.ObjectPath(path))
                log('bluealsa: removed %s' % path)

    @dbus.service.method(PROPS, in_signature='ss', out_signature='v')
    def Get(self, iface, name):
        return self.props()[name]

    @dbus.service.method(PROPS, in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return dbus.Dictionary(self.props(), signature='sv')

    def props(self):
        return {'Version': dbus.String('4.0.0'), 'Adapters': strv(['hci0']),
                'Profiles': strv(['a2dp-source', 'a2dp-sink']),
                'Codecs': strv(['A2DP-source:SBC', 'A2DP-sink:SBC'])}

    @dbus.service.method(BA_MGR, in_signature='', out_signature='a{oa{sv}}')
    def GetPCMs(self):
        return dbus.Dictionary({dbus.ObjectPath(p): dbus.Dictionary(x.ifaces[PCM], signature='sv')
                                for p, x in self.pcms.items()}, signature='oa{sv}')

    @dbus.service.signal(BA_MGR, signature='oa{sv}')
    def PCMAdded(self, path, props):
        pass

    @dbus.service.signal(BA_MGR, signature='o')
    def PCMRemoved(self, path):
        pass


# ---- test hooks ---------------------------------------------------------------------------------

class Fake(dbus.service.Object):
    """org.soundtester.Fake1: what the other side of a radio link would do."""

    def __init__(self, svc):
        super().__init__(svc.conn, '/org/soundtester/fake')
        self.svc = svc

    def device(self, address, create=True):
        address = str(address).upper()
        d = self.svc.devices.get(address)
        if d is None and create:
            w = world_entry(address)
            if w is None:
                raise dbus.DBusException('No such device in the fake world: ' + address,
                                         name='org.soundtester.Error.NoSuchDevice')
            d = self.svc.add_device(w)
        if d is None:
            raise dbus.DBusException('Device not present: ' + address,
                                     name='org.soundtester.Error.NoSuchDevice')
        return d

    @dbus.service.method(FAKE, in_signature='s', out_signature='s',
                         async_callbacks=('ok', 'fail'))
    def IncomingPair(self, address, ok=None, fail=None):
        """That device pairs with us, then (unless trusted) asks to use A2DP, then connects."""
        d = self.device(address)
        log('IncomingPair %s' % d.address)
        a = self.svc.adapter.p
        if not a['Powered']:
            return ok('rejected: adapter is off')
        if d.p['Paired']:
            return ok('already paired')
        if not a['Pairable']:
            log('  not pairable: rejected without asking the agent')
            return ok('rejected: not pairable')
        agent = self.svc.agent_for(None)

        def paired(e):
            if e is not None:
                return ok('pairing failed: ' + e.get_dbus_name())
            if d.p['Trusted']:
                d.connected()
                return ok('paired+connected')

            def authorized(*_):
                log('  agent AuthorizeService -> ok')
                d.connected()
                ok('paired+authorized+connected')

            def refused(e):
                log('  agent AuthorizeService -> %s' % e.get_dbus_name())
                ok('paired, service refused: ' + e.get_dbus_name())
            if agent is None:
                return ok('paired, service refused: no agent')
            self.svc.call_agent(agent, 'AuthorizeService', 'os',
                                (dbus.ObjectPath(d.path), dbus.String(UUID_A2DP)),
                                authorized, refused)
        d.run_pairing(agent, True, paired)

    @dbus.service.method(FAKE, in_signature='s', out_signature='s',
                         async_callbacks=('ok', 'fail'))
    def Display(self, address, ok=None, fail=None):
        d = self.device(address)
        agent = self.svc.agent_for(None)
        if agent is None:
            return ok('no agent')
        log('Display %s' % d.address)
        d.attempt += 1

        def done(e):
            d.update(DEVICE, Paired=dbus.Boolean(True), Bonded=dbus.Boolean(True))
            ok('paired')
        d.display_sequence(agent, done)

    @dbus.service.method(FAKE, in_signature='s', out_signature='')
    def DropLink(self, address):
        d = self.device(address, create=False)
        d.link_lost('DropLink: out of range')
        d.go_out_of_range()

    @dbus.service.method(FAKE, in_signature='s', out_signature='')
    def StartStream(self, address):
        d = self.device(address, create=False)
        if not d.p['Connected']:
            raise dbus.DBusException('Not connected', name='org.soundtester.Error.NotConnected')
        self.svc.bluealsa.remove_pcms(d, 'capture')
        self.svc.seq += 1
        self.svc.bluealsa.add_pcm(d, 'capture')

    @dbus.service.method(FAKE, in_signature='s', out_signature='')
    def StopStream(self, address):
        self.svc.bluealsa.remove_pcms(self.device(address, create=False), 'capture')

    @dbus.service.method(FAKE, in_signature='', out_signature='')
    def Reset(self):
        log('Reset')
        self.svc.reset()


# ---- the service --------------------------------------------------------------------------------

class Service:
    def __init__(self, conn):
        self.conn = conn
        self.agents = {}          # unique name -> (path, capability)
        self.default_agent = None
        self.devices = {}         # address -> Device
        self.seq = 0
        self.root = ObjectManager(self, '/')
        self.agent_mgr = AgentManager(self)
        self.adapter = Adapter(self)
        self.bluealsa = BluealsaManager(self)
        self.fake = Fake(self)
        self.root.add(self.agent_mgr)
        self.root.add(self.adapter)
        self.populate()
        # A client that goes away takes its agent and its scan with it, as in BlueZ.
        conn.add_signal_receiver(self.owner_changed, 'NameOwnerChanged', 'org.freedesktop.DBus',
                                 'org.freedesktop.DBus', '/org/freedesktop/DBus')

    def populate(self):
        for w in WORLD:
            if w.get('known'):
                self.add_device(w)

    def reset(self):
        for d in list(self.devices.values()):
            self.remove_device(d)
        a = self.adapter
        for sender in list(a.discovery):
            a.discovery.pop(sender)
        a.filters.clear()
        a.sync_discovering()
        a.cancel_timer('disc_timer')
        a.cancel_timer('pair_timer')
        a.update(ADAPTER, Powered=dbus.Boolean(True), PowerState=dbus.String('on'),
                 Discoverable=dbus.Boolean(False), DiscoverableTimeout=dbus.UInt32(180),
                 Pairable=dbus.Boolean(True), PairableTimeout=dbus.UInt32(0),
                 Alias=dbus.String('BlueZ 5.72'))
        self.populate()

    def add_device(self, w):
        d = Device(self, w)
        self.devices[w['address']] = d
        self.root.add(d)
        log('device %s (%s) appeared' % (w['address'], w['name']))
        return d

    def remove_device(self, d):
        d.attempt += 1
        d.cancel_forget()
        self.bluealsa.remove_pcms(d)
        self.devices.pop(d.address, None)
        self.root.remove(d)

    def agent_for(self, sender):
        if sender in self.agents:
            return (sender,) + self.agents[sender]
        if self.default_agent in self.agents:
            return (self.default_agent,) + self.agents[self.default_agent]
        return None

    def call_agent(self, agent, method, sig, args, reply, error):
        sender, path, _cap = agent
        log('-> agent %s %s%r' % (sender, method, tuple(str(a) for a in args)))
        self.conn.call_async(sender, path, AGENT, method, sig, args, reply, error,
                             timeout=AGENT_TIMEOUT_S)

    def drop_agent(self, sender):
        if self.agents.pop(sender, None) is not None:
            log('agent of %s unregistered' % sender)
        if self.default_agent == sender:
            self.default_agent = None

    def owner_changed(self, name, old, new):
        if name.startswith(':') and not new:
            self.drop_agent(name)
            self.adapter.client_gone(name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--address', required=True, help='the private bus to serve on')
    args = ap.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    conn = dbus.bus.BusConnection(args.address)
    # Gone with its bus: a fake outliving the bus it served would only confuse the next run.
    conn.set_exit_on_disconnect(True)
    svc = Service(conn)
    # Both well-known names on the one connection: a client addressing either reaches the same
    # objects, and each keeps to its own paths, so nothing can tell it is one process.
    names = [dbus.service.BusName(n, conn, do_not_queue=True)
             for n in ('org.bluez', 'org.bluealsa')]
    log('fake BlueZ + bluez-alsa on %s' % args.address)
    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    del names, svc


if __name__ == '__main__':
    sys.exit(main())
