"""HTML pages."""

from flask import Blueprint, redirect, render_template, session, url_for

from .. import config, db
from . import devices
from .auth import login_required

bp = Blueprint('views', __name__)


@bp.route('/', methods=['GET'])
def index():
    if 'username' in session:
        return redirect(url_for('views.track'))
    return redirect(url_for('auth.login'))


@bp.route('/track', methods=['GET'])
@login_required
def track():
    device = devices.from_request()
    if not device:
        return render_template('track.tpl', device=None, log={}, registration='',
                               operator='', engine_running=False,
                               engine_running_voltage=config.ENGINE_RUNNING_VOLTAGE,
                               engine_stopped_count=config.ENGINE_STOPPED_COUNT,
                               google_maps_api_key=config.MAPS_API_KEY)

    log = devices.latest_log(device) or {}
    log['combined_speed'] = devices.combined_speed(log)

    stamp = log.get('timestamp')
    log['display_date'] = stamp.strftime('%d.%m.%Y') if stamp else ''
    log['display_timestamp'] = stamp.strftime('%H:%M:%S') if stamp else ''

    return render_template(
        'track.tpl',
        device=device,
        log=log,
        registration=device.get('registration') or device.get('name') or '',
        operator=db.lookup_operator(log.get('mcc'), log.get('mnc')) or '',
        engine_running=devices.engine_running(device, log),
        engine_running_voltage=config.ENGINE_RUNNING_VOLTAGE,
        engine_stopped_count=config.ENGINE_STOPPED_COUNT,
        google_maps_api_key=config.MAPS_API_KEY,
    )
