"""Outbound notifications.

The reference deployment uses Pushover, but nothing above this module knows
that: callers pass a message, a title and a priority, and the configured
backend decides what to do with them.  ``none`` is a valid choice: every
notification is written to ``app.log`` whichever backend is set.

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
import queue
import threading
import time

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

# Deliveries happen on a thread of their own.  The callers are the UDP and TLS
# listeners and the web workers, and a request to Pushover or a webhook can take
# its whole 10 s timeout, which no tracker waiting seconds for a reply can wait.
_queue = queue.Queue(maxsize=1000)
_worker = None
_worker_lock = threading.Lock()


def _deliver():
    while True:
        handler, args = _queue.get()
        try:
            handler(*args)
        except Exception:
            logs.app.exception('notification failed: %s', args[1])
        finally:
            _queue.task_done()


def _start_worker():
    # Started on first use rather than at import: gunicorn forks its workers
    # after loading the app, and a thread does not survive the fork.
    global _worker
    with _worker_lock:
        if _worker is None or not _worker.is_alive():
            _worker = threading.Thread(target=_deliver, name='notify', daemon=True)
            _worker.start()


def wait_idle(timeout=10):
    """Wait until every queued notification has been attempted, or timeout."""
    deadline = time.monotonic() + timeout
    while _queue.unfinished_tasks and time.monotonic() < deadline:
        time.sleep(0.01)


def send(message, title='Tracker', priority=0, url=None, url_title=None):
    """Deliver a notification in the background.  Never raises and never
    blocks: the UDP listener calls this while a tracker waits a few seconds for
    its reply, so a slow or dead notification service must not hold it up."""
    logs.notify.info('%s, priority %d: %s', title, priority, message)
    handler = _BACKENDS.get(_backend)
    if handler is None:
        if _backend != 'none':
            logs.app.error('unknown notify backend %r', _backend)
        return
    _start_worker()
    try:
        _queue.put_nowait((handler, (message, title, priority, url, url_title)))
    except queue.Full:
        logs.app.error('notification queue full, dropped: %s', title)


# Deep links the firmware can request by prefixing an alert.  `locate` sends
# `google: <lat>,<lon>` and `tomtom` sends `tomtom: <lat>,<lon>`; turning those
# into a tappable link is the whole point of asking the device for a position.
_DEEP_LINKS = {
    'google: ': (lambda c: 'https://www.google.com/maps/search/?api=1&query=%s' % c,
                 'Open in Google Maps'),
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
