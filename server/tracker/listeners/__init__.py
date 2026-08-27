"""Transports.

Three ways a device can reach the server, in the order they were added:

``udp``
    ChaCha20-Poly1305 over plain UDP with a per-device pre-shared key.  One
    datagram out, one back.  The cheapest option in radio time and the only one
    that works on modems without enough spare flash for a TLS stack.

``tls``
    TLS 1.2 over TCP, offloaded to the modem.  Costs a handshake but gets
    certificate-based authentication and carries firmware downloads on the same
    port.

``dtls``
    DTLS 1.2 with Connection ID (RFC 9146).  Datagram-cheap like UDP, but the
    session survives the NAT rebinding that LTE-M RAI causes on every radio
    release, because records are matched by CID rather than source address.

All three converge on ``telemetry.process_lines`` once a batch of plaintext
lines has been attributed to a device.
"""
