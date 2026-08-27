"""Passkey (WebAuthn) authentication.

There are no passwords anywhere in this server.  A tracking server publishes
the location of a vehicle in real time, which makes a guessable or reused
credential an unusually bad trade; a platform authenticator with user
verification removes phishing and credential stuffing from the threat model
entirely.

Enrolment is out-of-band by design: ``tools/regtoken.py`` mints a single-use
24-hour URL that an operator delivers however they like.  There is no
self-service signup, because the only correct number of accounts on a personal
tracking server is the number the operator created deliberately.
"""

import base64
import json
import secrets
import time
import uuid
from base64 import urlsafe_b64decode
from functools import wraps

from flask import Blueprint, redirect, render_template, request, session, url_for
from webauthn import (
    generate_authentication_options,
    generate_registration_options,
    options_to_json,
    verify_authentication_response,
    verify_registration_response,
)
from webauthn.helpers.structs import (
    AuthenticationCredential,
    AuthenticatorAssertionResponse,
    AuthenticatorAttachment,
    AuthenticatorAttestationResponse,
    AuthenticatorSelectionCriteria,
    RegistrationCredential,
    ResidentKeyRequirement,
    UserVerificationRequirement,
)

from .. import config, db, logs
from . import audit, client_ip, error, json_response, ok

bp = Blueprint('auth', __name__)

REGISTRATION_TOKEN_TTL = 86400   # 24 hours
CHALLENGE_TTL = 300              # 5 minutes
MAX_FAILED_LOGINS = 5


def login_required(f):
    @wraps(f)
    def wrapper(*args, **kwargs):
        if 'username' not in session:
            return redirect(url_for('auth.login'))
        return f(*args, **kwargs)
    return wrapper


def rp_id():
    """The WebAuthn Relying Party ID — the registrable domain, no port."""
    return request.headers.get('X-Forwarded-Host') or request.host.split(':')[0]


def origin():
    hostname = rp_id()
    proto = request.headers.get('X-Forwarded-Proto', 'https')
    port_header = request.headers.get('X-Forwarded-Port')
    port = int(port_header) if port_header else (443 if proto == 'https' else 80)
    if (proto == 'https' and port == 443) or (proto == 'http' and port == 80):
        return '%s://%s' % (proto, hostname)
    return '%s://%s:%d' % (proto, hostname, port)


def _session_id():
    if 'session_id' not in session:
        session['session_id'] = str(uuid.uuid4())
    return session['session_id']


# -- login -------------------------------------------------------------------

@bp.route('/login', methods=['GET'])
def login():
    if 'username' in session:
        return redirect(url_for('views.track'))
    return render_template('login.tpl')


@bp.route('/logout', methods=['GET'])
@login_required
def logout():
    audit('logout', session.get('username', ''))
    session.pop('username', None)
    return redirect(url_for('auth.login'))


# -- registration ------------------------------------------------------------

@bp.route('/regoptions', methods=['POST'])
def regoptions():
    username = request.form.get('username')
    token = request.form.get('token')
    if not username or not token:
        audit('regoptions-error', 'missing username or token')
        return error('missing username or token')

    row = db.web.one(
        'SELECT * FROM `registration` WHERE `username` = %s AND `token` = %s',
        (username, token),
    )
    if not row:
        audit('regoptions-error', 'invalid username or token')
        return error('invalid username or token')

    if time.time() - row['timestamp'] >= REGISTRATION_TOKEN_TTL:
        db.web.query('DELETE FROM `registration` WHERE `id` = %s', (row['id'],))
        audit('regoptions-error', 'token expired')
        return error('token expired')

    options = generate_registration_options(
        rp_id=rp_id(),
        rp_name=rp_id(),
        # The WebAuthn user handle.  Deliberately not the username or a
        # database id: it is stored on the authenticator and shown in the
        # platform's passkey list, so it should carry no meaning that outlives
        # the credential.
        user_id=secrets.token_urlsafe(24),
        user_name=username,
        authenticator_selection=AuthenticatorSelectionCriteria(
            # A platform authenticator with user verification and a discoverable
            # credential: the device holding the key must confirm the user, and
            # the key cannot be exported to be phished.
            authenticator_attachment=AuthenticatorAttachment.PLATFORM,
            user_verification=UserVerificationRequirement.REQUIRED,
            resident_key=ResidentKeyRequirement.REQUIRED,
        ),
    )
    options_json = options_to_json(options)
    now = int(time.time())
    db.web.query(
        'INSERT INTO `regoptions` (`session_id`, `regoptions`, `timestamp`) '
        'VALUES (%s, %s, %s) '
        'ON DUPLICATE KEY UPDATE `regoptions` = %s, `timestamp` = %s',
        (_session_id(), options_json, now, options_json, now),
    )

    audit('regoptions-success', username)
    return json_response({'status': 'ok', 'regoptions': json.loads(options_json)})


@bp.route('/register', methods=['GET', 'POST'])
def register():
    username = request.values.get('username')
    token = request.values.get('token')

    row = db.web.one(
        'SELECT * FROM `registration` WHERE `username` = %s AND `token` = %s',
        (username, token),
    )
    if not row:
        if request.method == 'POST':
            audit('register-error', 'invalid username or token')
            return error('invalid username or token')
        audit('registration-link-error', 'invalid username or token')
        return redirect(url_for('auth.login'))

    _session_id()

    if request.method == 'GET':
        audit('register-begin', username)
        return render_template('register.tpl', username=username)

    stored = db.web.one('SELECT * FROM `regoptions` WHERE `session_id` = %s',
                        (session['session_id'],))
    if not stored:
        audit('register-error', 'missing regoptions')
        return error('missing regoptions')
    if time.time() - stored['timestamp'] > CHALLENGE_TTL:
        audit('register-error', 'regoptions expired')
        return error('regoptions expired')

    data = request.get_json(silent=True) or {}

    try:
        credential = RegistrationCredential(
            id=data['id'],
            raw_id=urlsafe_b64decode(data['rawId']),
            response=AuthenticatorAttestationResponse(
                attestation_object=urlsafe_b64decode(
                    data['response']['attestationObject']),
                client_data_json=urlsafe_b64decode(
                    data['response']['clientDataJSON']),
            ),
            type=data['type'],
        )
        verification = verify_registration_response(
            credential=credential,
            expected_challenge=json.loads(stored['regoptions'])['challenge'].encode(),
            expected_origin=origin(),
            expected_rp_id=rp_id(),
        )
    except Exception as e:
        audit('register-error', str(e))
        return error(str(e))

    # Burn the invitation, then replace any existing credential for this
    # username — re-registering is how a user recovers a lost authenticator.
    db.web.query(
        'DELETE FROM `registration` WHERE `username` = %s AND `token` = %s',
        (username, token),
    )
    db.web.query('DELETE FROM `user` WHERE `username` = %s', (username,))
    db.web.query(
        'INSERT INTO `user` (`username`, `user_id`, `credential`) VALUES (%s, %s, %s)',
        (
            username,
            credential.id,
            json.dumps({
                'credential_id': base64.b64encode(credential.raw_id).decode('ascii'),
                'public_key': base64.b64encode(
                    verification.credential_public_key).decode('ascii'),
                'sign_count': verification.sign_count,
            }),
        ),
    )

    audit('register-success', username)
    return ok()


# -- authentication ----------------------------------------------------------

def _rate_limited(ip):
    """Per-IP limit on challenge issuance.

    The challenge endpoint is the only unauthenticated one that touches the
    user table, so it is where an attacker would go to enumerate usernames or
    grind at a stolen authenticator.
    """
    row = db.web.one('SELECT * FROM `authoptions_ip` WHERE `ip` = %s', (ip,))
    now = int(time.time())

    if row and row['count'] >= config.RATE_LIMIT_REQUEST_COUNT:
        if now - row['last_request_timestamp'] < config.RATE_LIMIT_RESET_PERIOD:
            return True
        db.web.query(
            'UPDATE `authoptions_ip` SET `count` = 1, `last_request_timestamp` = %s '
            'WHERE `ip` = %s', (now, ip))
        return False

    if row:
        db.web.query(
            'UPDATE `authoptions_ip` SET `count` = `count` + 1, '
            '`last_request_timestamp` = %s WHERE `ip` = %s', (now, ip))
    else:
        db.web.query(
            'INSERT INTO `authoptions_ip` (`ip`, `count`, `last_request_timestamp`) '
            'VALUES (%s, 1, %s)', (ip, now))
    return False


@bp.route('/authoptions', methods=['POST'])
def authoptions():
    _session_id()
    ip = client_ip()

    if _rate_limited(ip):
        audit('authoptions-rate-limit', 'IP %s exceeded rate limit' % ip)
        return json_response({'status': 'error', 'message': 'too many requests'}, 429)

    data = request.get_json(silent=True) or {}
    username = data.get('username')
    if not username:
        audit('authoptions-error', 'missing username')
        return error('invalid request')

    user = db.web.one('SELECT * FROM `user` WHERE `username` = %s', (username,))
    if not user:
        audit('authoptions-error', 'user not found')
        return json_response({'status': 'error', 'message': 'user not found'}, 401)
    if user['locked']:
        audit('authoptions-error', 'account locked')
        return json_response({'status': 'error', 'message': 'account locked'}, 401)

    options = generate_authentication_options(
        rp_id=rp_id(),
        user_verification=UserVerificationRequirement.PREFERRED,
    )

    db.web.query('DELETE FROM `authoptions` WHERE `session_id` = %s',
                 (session['session_id'],))
    db.web.query(
        'INSERT INTO `authoptions` (`user_id`, `session_id`, `authoptions`, '
        '`timestamp`, `useragent`, `ipaddr`) VALUES (%s, %s, %s, %s, %s, %s)',
        (
            user['user_id'], session['session_id'],
            base64.b64encode(options.challenge).decode('ascii'),
            int(time.time()), request.headers.get('User-Agent', ''), ip,
        ),
    )

    audit('authoptions-success', username)
    return json_response({
        'status': 'ok',
        'authoptions': {
            'challenge': [int(b) for b in options.challenge],
            'allow_credentials': [],
        },
    })


@bp.route('/authenticate', methods=['POST'])
def authenticate():
    data = request.get_json(silent=True) or {}

    pending = db.web.one('SELECT * FROM `authoptions` WHERE `session_id` = %s',
                         (session.get('session_id'),))
    if not pending:
        audit('login-error', 'no challenge for session')
        return json_response({'status': 'error', 'message': 'no challenge for session'}, 401)

    user = db.web.one('SELECT * FROM `user` WHERE `user_id` = %s',
                      (data.get('user_id'),))
    if not user:
        audit('login-error', 'user not found')
        return json_response({'status': 'error', 'message': 'user not found'}, 401)
    if user['locked']:
        audit('login-error', 'account locked')
        return json_response({'status': 'error', 'message': 'account locked'}, 401)

    # The challenge is bound to the user agent and IP that asked for it, so a
    # challenge intercepted in flight cannot be completed from elsewhere.
    if pending['useragent'] != request.headers.get('User-Agent', ''):
        audit('login-error', 'useragent mismatch')
        return json_response({'status': 'error', 'message': 'useragent mismatch'}, 401)
    if pending['ipaddr'] != client_ip():
        audit('login-error', 'IP mismatch')
        return json_response({'status': 'error', 'message': 'IP mismatch'}, 401)
    if time.time() - pending['timestamp'] >= CHALLENGE_TTL:
        db.web.query('DELETE FROM `authoptions` WHERE `session_id` = %s',
                     (session['session_id'],))
        audit('login-error', 'challenge expired')
        return json_response({'status': 'error', 'message': 'challenge expired'}, 401)

    stored = json.loads(user['credential'])
    assertion = data.get('authentication_data') or {}

    try:
        credential = AuthenticationCredential(
            id=data.get('user_id'),
            raw_id=urlsafe_b64decode(assertion['rawId']),
            response=AuthenticatorAssertionResponse(
                authenticator_data=urlsafe_b64decode(
                    assertion['response']['authenticatorData']),
                client_data_json=urlsafe_b64decode(
                    assertion['response']['clientDataJSON']),
                signature=urlsafe_b64decode(assertion['response']['signature']),
            ),
            type='public-key',
        )
        verify_authentication_response(
            credential=credential,
            expected_challenge=base64.b64decode(pending['authoptions']),
            expected_origin=origin(),
            expected_rp_id=rp_id(),
            credential_public_key=base64.b64decode(stored['public_key']),
            credential_current_sign_count=stored['sign_count'],
        )
    except Exception as e:
        # Every way this can fail — a bad signature, a malformed assertion, a
        # missing field — is a failed login and is counted as one.  The library
        # raises a family of subclasses whose membership varies between
        # versions, so there is nothing narrower worth catching here.
        failed = user['failed_login_count'] + 1
        db.web.query(
            'UPDATE `user` SET `failed_login_count` = %s, `locked` = %s WHERE `id` = %s',
            (failed, 1 if failed >= MAX_FAILED_LOGINS else user['locked'], user['id']),
        )
        audit('login-error', 'verification failed: %s (count=%d)' % (e, failed))
        logs.app.warning('failed login for %s from %s', user['username'], client_ip())
        return json_response({'status': 'error', 'message': 'authentication failed'}, 401)

    db.web.query('DELETE FROM `authoptions` WHERE `session_id` = %s',
                 (session['session_id'],))
    db.web.query('UPDATE `user` SET `failed_login_count` = 0 WHERE `id` = %s',
                 (user['id'],))
    db.web.query('UPDATE `authoptions_ip` SET `count` = 0 WHERE `ip` = %s',
                 (client_ip(),))

    session['username'] = user['username']
    audit('login-success', user['username'])
    return ok({'message': 'authentication successful'})
