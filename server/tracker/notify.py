"""Outbound notifications.

The reference deployment uses Pushover, but nothing above this module knows
that: callers pass a message, a title and a priority, and the configured
backend decides what to do with them.  ``none`` is a valid choice — the server
runs fine with alerts only going to ``udp.log``.

Backends are selected in config.yaml::

    notify:
      backend: pushover        # pushover | webhook | none
      user: ...
      app: ...

Priorities follow Pushover's scale because it is the one with the most
semantics: -1 quiet, 0 normal, 2 requires acknowledgement.  A webhook backend
gets the number verbatim and can map it however it likes.
"""

import json

import requests

from . import config, logs

PUSHOVER_URL = 'https://api.pushover.net/1/messages.json'

_backend = (config.NOTIFY.get('backend') or 'none').lower()


def _send_pushover(message, title, priority, url, url_title):
    payload = {
        'token': config.NOTIFY['app'],
        'user': config.NOTIFY['user'],
        'message': message,
        'title': title,
        'priority': priority,
    }
    if priority == 2:
        # Pushover requires both when an acknowledgement is demanded.
        payload['retry'] = int(config.NOTIFY.get('retry', 30))
        payload['expire'] = int(config.NOTIFY.get('expire', 300))
    if url:
        payload['url'] = url
    if url_title:
        payload['url_title'] = url_title
    response = requests.post(PUSHOVER_URL, data=payload, timeout=10)
    response.raise_for_status()


def _send_webhook(message, title, priority, url, url_title):
    body = {
        'title': title,
        'message': message,
        'priority': priority,
        'url': url,
        'url_title': url_title,
    }
    headers = {'Content-Type': 'application/json'}
    token = config.NOTIFY.get('token')
    if token:
        headers['Authorization'] = 'Bearer %s' % token
    response = requests.post(
        config.NOTIFY['url'],
        data=json.dumps(body),
        headers=headers,
        timeout=10,
    )
    response.raise_for_status()


_BACKENDS = {
    'pushover': _send_pushover,
    'webhook': _send_webhook,
}


def send(message, title='Tracker', priority=0, url=None, url_title=None):
    """Deliver a notification.  Never raises — a dead notification service
    must not cost a telemetry record."""
    handler = _BACKENDS.get(_backend)
    if handler is None:
        if _backend != 'none':
            logs.app.error('unknown notify backend %r', _backend)
        return
    try:
        handler(message, title, priority, url, url_title)
    except Exception:
        logs.app.exception('notification failed: %s', title)


# Deep links the firmware can request by prefixing an alert.  `locate` sends
# `google: <lat>,<lon>` and `tomtom` sends `tomtom: <lat>,<lon>`; turning those
# into a tappable link is the whole point of asking the device for a position.
_DEEP_LINKS = {
    'google: ': (lambda c: 'comgooglemaps://?q=%s' % c, 'Open in Google Maps'),
    'tomtom: ': (lambda c: 'tomtomgo://x-callback-url/navigate?destination=%s' % c,
                 'Open in TomTom'),
}


def device_alert(device_name, alert_msg, priority):
    """Relay a device-originated alert, expanding any location deep link."""
    url = url_title = None
    for prefix, (build, label) in _DEEP_LINKS.items():
        if alert_msg.startswith(prefix):
            url = build(alert_msg[len(prefix):].strip())
            url_title = label
            break
    send('%s: %s' % (device_name, alert_msg), priority=priority,
         url=url, url_title=url_title)
