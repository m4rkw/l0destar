"""l0destar tracking server.

Layout::

    config.py      configuration loading
    db.py          MySQL handles and device/operator lookups
    logs.py        per-transport log channels
    notify.py      pluggable outbound notifications
    telemetry.py   record parsing, storage, journeys, response building
    firmware.py    OTA manifests and the firmware HTTP server
    listeners/     UDP (ChaCha20-Poly1305), TLS and DTLS transports
    web/           Flask application, passkey auth, JSON API

The listeners and the web application share the telemetry layer and nothing
else; either can be run without the other.
"""

__version__ = '0.1.0'
