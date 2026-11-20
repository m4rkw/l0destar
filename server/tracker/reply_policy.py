"""When a reply carries the device's whole configuration and when it is short.

Every datagram is answered, and the answer used to carry everything the unit
might need to converge on (``backup= fota= gb= gw= le= tu= im= gx= os=``,
about 80 bytes for the car).  The drive stream sends several datagrams a second
and every one of them paid for it, in both directions of a metered SIM: the
reply was a third of the stream's traffic.

The firmware acts only on the tokens a reply carries, so a reply of just
``1,<interval>,<movement_alarm>[,<commands>]`` changes nothing it already has.
The whole set goes out when it is news:

* the first reply to a device since the server started (nothing known);
* the datagram came from a unit that has just booted (it carries ``fw=``);
* the settings differ from the ones the last full reply carried (a setting
  changed in the app, a firmware version was published, a switch moved);
* as a safety net, once REFRESH_S have passed since the last full one, because
  UDP can lose a reply that carried a change and nothing would send it again.

Commands queued for the unit always ride along: they are part of the short
reply, not of the tail.

Standard library only: the same file runs inside the tracker package and as a
module beside the live main.py.
"""
import threading
import time

REFRESH_S = 60


class ReplyGate:
    def __init__(self, refresh_s=REFRESH_S, clock=time.monotonic):
        self._refresh_s = refresh_s
        self._clock = clock
        self._sent = {}                    # device id -> (tail, when it went)
        self._lock = threading.Lock()

    def full(self, device_id, tail, boot=False):
        """Whether this reply should carry ``tail``, the settings part."""
        now = self._clock()
        with self._lock:
            known = self._sent.get(device_id)
            send = (boot or known is None or known[0] != tail
                    or now - known[1] >= self._refresh_s)
            if send:
                self._sent[device_id] = (tail, now)
        return send


gate = ReplyGate()
