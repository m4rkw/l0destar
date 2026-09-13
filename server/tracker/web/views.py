"""HTML pages."""

import datetime

from flask import Blueprint, redirect, render_template, url_for

from .. import config, db
from . import devices
from .auth import current_user, login_required

bp = Blueprint('views', __name__)


@bp.route('/', methods=['GET'])
def index():
    """Land on a map when it is clear which vehicle, else on the list."""
    if current_user() is None:
        return redirect(url_for('auth.login'))
    device = devices.default_device()
    if device:
        return redirect(url_for('views.track', imei=device['imei']))
    return redirect(url_for('views.device_list'))


def age(stamp, now=None):
    """How long ago a record arrived, in the coarsest unit that fits."""
    if stamp is None:
        return 'never'
    seconds = ((now or datetime.datetime.now()) - stamp).total_seconds()
    for unit, size in (('d', 86400), ('h', 3600), ('m', 60)):
        if seconds >= size:
            return '%d%s ago' % (seconds // size, unit)
    return 'just now'


@bp.route('/devices', methods=['GET'])
@login_required
def device_list():
    """Every enrolled device, each linking to its own map."""
    listed = []
    for device in devices.enrolled():
        log = devices.latest_log(device) or {}
        item = devices.summary(device, log=log)
        if not log:
            item['state'] = '-'
        elif devices.engine_running(device, log):
            item['state'] = 'engine on'
        elif log.get('ignition_state') == 1:
            item['state'] = 'ignition on'
        else:
            item['state'] = 'ignition off'
        item['age'] = age(log.get('timestamp'))
        listed.append(item)
    return render_template('devices.tpl', devices=listed)


@bp.route('/track', methods=['GET'])
@login_required
def track():
    device = devices.from_request()
    if not device:
        # The request named a device that is not enrolled, or several are
        # enrolled and none is the default: let the user choose.
        return redirect(url_for('views.device_list'))

    log = devices.latest_log(device) or {}
    log['combined_speed'] = devices.combined_speed(log)

    stamp = log.get('timestamp')
    log['display_date'] = stamp.strftime('%d.%m.%Y') if stamp else ''
    log['display_timestamp'] = stamp.strftime('%H:%M:%S') if stamp else ''

    return render_template(
        'track.tpl',
        device=device,
        device_count=devices.count(),
        log=log,
        registration=device.get('registration') or device.get('name') or '',
        operator=db.lookup_operator(log.get('mcc'), log.get('mnc')) or '',
        engine_running=devices.engine_running(device, log),
        engine_running_voltage=config.ENGINE_RUNNING_VOLTAGE,
        engine_stopped_count=config.ENGINE_STOPPED_COUNT,
        google_maps_api_key=config.MAPS_API_KEY,
    )
