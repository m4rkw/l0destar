"""Flask application factory."""

import datetime
import json

from flask import Flask, Response, request, session
from flask_sock import Sock
from werkzeug.middleware.proxy_fix import ProxyFix

from .. import config, logs

sock = Sock()


def json_response(payload, status=200):
    return Response(
        json.dumps(payload, separators=(',', ':')),
        status=status,
        headers={'Content-Type': 'application/json'},
    )


def ok(data=None):
    payload = {'status': 'ok'}
    if data:
        payload.update(data)
    return json_response(payload)


def error(message, status=400):
    return json_response({'status': 'error', 'message': message}, status)


def unauthorised(message='unauthorised'):
    return json_response({'status': 'error', 'message': message}, 401)


def client_ip():
    return request.headers.get('X-Forwarded-For', request.remote_addr or '')


def audit(action, detail=''):
    """Append to the audit log.

    Kept as a plain append-only file rather than a table: it has to survive
    the database being the thing that broke, and it is the record consulted
    after a suspected compromise.
    """
    stamp = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    line = '%s - %s [%s] - [%s] - %s\n' % (
        client_ip(), session.get('username', ''), stamp, action, detail)
    try:
        with open(logs.AUDIT_PATH, 'a') as f:
            f.write(line)
    except Exception:
        logs.app.exception('audit log write failed')


def create_app():
    app = Flask(
        __name__,
        template_folder='../../templates',
        static_folder='../../static',
        static_url_path='/static',
    )

    # The app is expected to sit behind a reverse proxy terminating TLS.  The
    # forwarded headers are what WebAuthn's origin and RP-ID checks are derived
    # from, so they have to be trusted here — which in turn means the proxy
    # must overwrite rather than append them.
    app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_host=1, x_port=1, x_proto=1)

    app.secret_key = config.SESSION_SECRET
    app.permanent_session_lifetime = datetime.timedelta(
        days=config.SESSION_LIFETIME_DAYS)
    app.config['MAX_CONTENT_LENGTH'] = 1024 * 1024
    app.config['SESSION_COOKIE_HTTPONLY'] = True
    app.config['SESSION_COOKIE_SAMESITE'] = 'Lax'
    app.config['SESSION_COOKIE_SECURE'] = bool(config.get('secure_cookies', True))

    # Flask's default select_autoescape() covers .html and friends but not
    # .tpl, which would leave every telemetry field a stored-XSS vector.
    # Escape everything regardless of extension.
    app.jinja_env.autoescape = True

    @app.before_request
    def _permanent_session():
        session.permanent = True

    @app.after_request
    def _security_headers(response):
        response.headers.setdefault('X-Content-Type-Options', 'nosniff')
        response.headers.setdefault('X-Frame-Options', 'DENY')
        response.headers.setdefault('Referrer-Policy', 'same-origin')
        return response

    from . import api, auth, views
    app.register_blueprint(views.bp)
    app.register_blueprint(auth.bp)
    app.register_blueprint(api.bp)

    sock.init_app(app)
    from . import stream  # noqa: F401  (registers the WebSocket route)

    return app
