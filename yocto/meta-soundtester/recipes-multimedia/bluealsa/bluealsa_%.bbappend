# bluez-alsa carries the Bluetooth audio: it registers the A2DP endpoints with BlueZ, does the SBC
# encoding and decoding, and hands soundtesterd an ALSA PCM per link (bluealsa:DEV=...,PROFILE=a2dp).
# Both roles, as meta-multimedia already defaults to: a2dp-source plays to a speaker, a2dp-sink takes
# a phone's audio in.
#
# --keep-alive holds a link's transport for a few seconds after its PCM closes. The daemon reopens
# a PCM whenever a write or read fails, and without it each reopen would release the transport and
# acquire it again — a second of silence and, on some speakers, a "connected" chime each time.
SYSTEMD_BLUEALSA_ARGS = "-p a2dp-source -p a2dp-sink --keep-alive=5"
