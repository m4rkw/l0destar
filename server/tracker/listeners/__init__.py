"""Device-facing listeners.

``udp``
    Telemetry: ChaCha20-Poly1305 over plain UDP with a per-device pre-shared
    key.  One datagram out, one back, and no session to set up or keep alive
    between wakes, which is the cheapest option in radio time.

``tls``
    Firmware downloads: TLS 1.2 over TCP, terminated by the modem, with a
    certificate issued by the CA compiled into the firmware.

Decrypted telemetry goes to ``telemetry.process_lines``, and firmware requests
to ``firmware.serve_http``.
"""
