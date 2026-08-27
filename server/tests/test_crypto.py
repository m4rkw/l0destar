"""The UDP transport's authenticated envelope.

What matters here is the failure cases, not the happy path: this is the only
transport where the server accepts a datagram from anyone who can reach the
port, so every one of these rejections is load-bearing.
"""

import secrets

import pytest

from tracker.listeners import udp

IMEI = '350000000000000'


@pytest.fixture
def device():
    return {'id': 1, 'imei': IMEI, 'psk': secrets.token_hex(32), 'name': 'test'}


def test_key_must_be_32_bytes(device):
    assert udp._aead(device) is not None
    assert udp._aead({'psk': 'short'}) is None
    assert udp._aead({'psk': ''}) is None
    assert udp._aead({'psk': 'z' * 64}) is None      # right length, not hex
    assert udp._aead({}) is None


def test_round_trip(device):
    aead = udp._aead(device)
    nonce = secrets.token_bytes(12)
    plaintext = b'12/08/26,17:30:45+04,0,0,0,0,0,0,0,12.1,0,3,0'
    imei_bytes = IMEI.encode()
    sealed = aead.encrypt(nonce, plaintext, imei_bytes)
    assert aead.decrypt(nonce, sealed, imei_bytes) == plaintext


def test_tampering_is_detected(device):
    aead = udp._aead(device)
    nonce = secrets.token_bytes(12)
    sealed = bytearray(aead.encrypt(nonce, b'payload', IMEI.encode()))
    sealed[-1] ^= 0xFF
    with pytest.raises(Exception):
        aead.decrypt(nonce, bytes(sealed), IMEI.encode())


def test_imei_is_bound_as_aad(device):
    # Without this, a datagram captured from one device could be replayed at
    # another that happened to share a key.
    aead = udp._aead(device)
    nonce = secrets.token_bytes(12)
    sealed = aead.encrypt(nonce, b'payload', IMEI.encode())
    with pytest.raises(Exception):
        aead.decrypt(nonce, sealed, b'999999999999999')


def test_response_is_bound_to_request_nonce(device):
    # A response resealed from an earlier exchange must not verify, or an
    # attacker could replay an old settings reply to revert a device.
    aead = udp._aead(device)
    request_nonce = secrets.token_bytes(12)
    response = udp.encrypt_response(device, '1,3600,0,1', request_nonce)

    assert aead.decrypt(response[:12], response[12:],
                        IMEI.encode() + request_nonce) == b'1,3600,0,1'

    with pytest.raises(Exception):
        aead.decrypt(response[:12], response[12:],
                     IMEI.encode() + secrets.token_bytes(12))


def test_response_needs_a_key():
    assert udp.encrypt_response({'imei': IMEI, 'psk': 'bad'}, 'x', b'0' * 12) is None


@pytest.mark.parametrize('raw', [
    b'',
    b'\x0f123',                          # truncated
    bytes([99]) + b'x' * 60,             # imei_len out of range
    bytes([13]) + b'x' * 60,             # imei_len too short
    bytes([15]) + b'notdigits12345' + b'x' * 40,
])
def test_malformed_envelopes_rejected(raw):
    # Every failure mode returns the same thing, so responses cannot be used
    # to work out which IMEIs are enrolled.
    assert udp.decrypt_request(raw) == (None, None, None)


def test_failure_logging_is_rate_limited():
    ip = '203.0.113.%d' % secrets.randbelow(200)
    assert udp._should_log_failure(ip) is True     # first in window
    logged = sum(1 for _ in range(50) if udp._should_log_failure(ip))
    # 1-in-20 after the first, so far fewer than one line per packet.
    assert 0 < logged < 10
