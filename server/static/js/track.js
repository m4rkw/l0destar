var map = null;
var pos = null;
var marker = null;
var accuracyCircle = null;
var engineRunning = false;

// The speed to show: the ECU's road speed when the device reported one, else
// GNSS.  The server sends it as combined_speed; points from before that
// column existed only carry the GNSS figure.
function speedOf(d) {
  var v = d['combined_speed'];
  if (v === undefined || v === null || v === '') v = d['speed'];
  return parseFloat(v);
}
var belowVoltageCount = 0;

$.urlParam = function(name){
  var results = new RegExp('[\?&]' + name + '=([^&#]*)').exec(window.location.href);
  if (results==null){
    return null;
  } else {
   return decodeURI(results[1]) || 0;
  }
}

function initMap() {
  pos = {lat: 0, lng: 0}

  map = new google.maps.Map(document.getElementById('map'), {
    //center: pos,
    zoom: 15
  });

  var image = '/static/img/car100.png';
  var icon = {
    path: image,
    scale: 1,
    rotation: 90
  }

  marker = new google.maps.Marker({
    position: map.getCenter(),
    map: map,
    title: 'vehicle',
    icon: {
      path: google.maps.SymbolPath.FORWARD_CLOSED_ARROW,
      scale: 4,
      rotation: 0
    }
  });

  setTimeout(function() {
    updateMap(map, marker);
  }, 500);
}

function applyPosition(data) {
  if (data.track_mode !== undefined && data.track_mode !== null) {
    setTrackViewFromServer(parseInt(data.track_mode) === 1);
  }
  if (data.ping) return;
  if (data.track) {
    updateTrack(data);
  }
  var pos = {lat: parseFloat(data['latitude']), lng: parseFloat(data['longitude'])};
  map.center = pos;
  marker.setIcon({
    path: google.maps.SymbolPath.FORWARD_CLOSED_ARROW,
    scale: 4,
    rotation: parseFloat(data['heading'])
  });
  marker.setPosition(pos);
  map.setCenter(pos);
  $('a.gps-link').attr('href', 'https://maps.google.co.uk/maps/place/' + data['latitude'] + ',' + data['longitude'] + '/');
  $('span.speed').text(Math.round(speedOf(data)));
  $('span.altitude').text(data['altitude']);
  $('span.heading').text(data['heading']);

  var engine_running_voltage = parseFloat($('input#engine_running_voltage').val());
  var engine_stopped_count = parseInt($('input#engine_stopped_count').val());

  // Engine RPM from the ECU, when the device reports one, settles it
  // outright: charging voltage is only a proxy, and a noisy or load-shed
  // rail reads below the threshold on a car that is being driven.
  // Hysteresis applies to the voltage fallback only: require
  // engine_stopped_count consecutive below-threshold readings before
  // transitioning from engine on to ignition on.
  var rpm = data['obd_rpm'];
  if (data['ignition_state'] != 1) {
      engineRunning = false;
      belowVoltageCount = engine_stopped_count;
  } else if (rpm !== undefined && rpm !== null && rpm !== '') {
      engineRunning = parseInt(rpm) > 0;
      belowVoltageCount = engineRunning ? 0 : engine_stopped_count;
  } else if (parseFloat(data['battery_level']) >= engine_running_voltage) {
      belowVoltageCount = 0;
      engineRunning = true;
  } else {
      belowVoltageCount++;
      if (belowVoltageCount >= engine_stopped_count)
          engineRunning = false;
  }

  if (engineRunning) {
      $('div.ignition').text('engine on');
  } else if (data['ignition_state'] == 1) {
      $('div.ignition').text('ignition on');
  } else {
      $('div.ignition').text('ignition off');
  }

  $('span.voltage').text(parseFloat(data['battery_level']).toFixed(2) + 'v');

  updateObd(data);
  updateImu(data);

  /* possible alternator failure — only flag when engine is genuinely not running */
  if (speedOf(data) >= 1 && data['ignition_state'] == 1 && !engineRunning) {
      $('span.voltage').addClass('red');
  } else {
      $('span.voltage').removeClass('red');
  }

  var ts = data['timestamp'];
  var d = ts.split(' ');
  $('span.date').text(d[0]);
  $('span.timestamp').text(d[1]);
  $('span.network').text(data['network']);
  $('span.operator').text(data['operator'] || '');
  $('span.rat').text(data['rat'] || '');

  // show accuracy circle for cell-based positions
  if (data['cell_accuracy']) {
    var radius = parseFloat(data['cell_accuracy']);
    if (accuracyCircle) {
      accuracyCircle.setCenter(pos);
      accuracyCircle.setRadius(radius);
    } else {
      accuracyCircle = new google.maps.Circle({
        map: map,
        center: pos,
        radius: radius,
        fillColor: '#4285F4',
        fillOpacity: 0.15,
        strokeColor: '#4285F4',
        strokeOpacity: 0.4,
        strokeWeight: 1,
      });
    }
    accuracyCircle.setVisible(true);
  } else if (accuracyCircle) {
    accuracyCircle.setVisible(false);
  }
}

// -- ECU readings panel --
// Every OBD column is independently optional and only K-wire builds report
// any of them, so the panel is shown when the record carries at least one
// and hidden otherwise (no K interface, ignition off, or an old journey
// from before the columns existed).  A field the ECU skipped this cycle
// shows as a dash rather than a stale value.

// SAE J1979 PID 03 byte A: one bit set for the fuel system's loop state.
// Kept short so it fits a fifth of the panel width in courier.
var FUEL_STATUS = {
  1:  'warmup',     // open loop, engine not yet warm
  2:  'closed',     // closed loop, using O2 feedback
  4:  'open',       // open loop from load or deceleration
  8:  'fault',      // open loop because of a system fault
  16: 'o2 fault',   // closed loop but an O2 sensor is faulty
};

function obdNum(v) {
  if (v === undefined || v === null || v === '') return null;
  v = parseFloat(v);
  return isNaN(v) ? null : v;
}

function obdSet(key, text, cls) {
  var el = $('.obd-value[data-obd="' + key + '"]');
  el.removeClass('empty warn');
  if (text === null) {
    el.text('-').addClass('empty');
  } else {
    el.text(text);
    if (cls) el.addClass(cls);
  }
}

function obdSigned(v, digits, unit) {
  if (v === null) return null;
  return (v > 0 ? '+' : '') + v.toFixed(digits) + unit;
}

function updateObd(data) {
  var rpm = obdNum(data['obd_rpm']);
  var coolant = obdNum(data['obd_coolant']);
  var intake = obdNum(data['obd_intake']);
  var load = obdNum(data['obd_load']);
  var throttle = obdNum(data['obd_throttle']);
  var maf = obdNum(data['obd_maf']);
  var timing = obdNum(data['obd_timing']);
  var stft = obdNum(data['obd_stft']);
  var ltft = obdNum(data['obd_ltft']);
  var fuel = obdNum(data['obd_fuel_status']);
  var mil = obdNum(data['obd_mil']);
  var dtc = obdNum(data['obd_dtc_count']);

  var any = [rpm, coolant, intake, load, throttle, maf, timing, stft, ltft,
             fuel, mil].some(function(v) { return v !== null; });
  if (!any || trackMode) {          /* the dashboard carries all of this */
    $('#obd').hide();
    return;
  }
  $('#obd').show();

  obdSet('rpm', rpm === null ? null : Math.round(rpm).toString());
  // Coolant well above a normal running temperature is worth a glance.
  obdSet('coolant', coolant === null ? null : Math.round(coolant) + '\u00b0C',
         coolant !== null && coolant >= 110 ? 'warn' : '');
  obdSet('intake', intake === null ? null : Math.round(intake) + '\u00b0C');
  obdSet('load', load === null ? null : load.toFixed(1) + '%');
  obdSet('throttle', throttle === null ? null : throttle.toFixed(1) + '%');
  obdSet('maf', maf === null ? null : maf.toFixed(2));
  obdSet('timing', timing === null ? null : obdSigned(timing, 1, '\u00b0'));
  // Trims beyond +/-10% for long suggest the ECU is compensating for
  // something (leak, injector, sensor); flag them.
  obdSet('stft', obdSigned(stft, 1, '%'),
         stft !== null && Math.abs(stft) >= 10 ? 'warn' : '');
  obdSet('ltft', obdSigned(ltft, 1, '%'),
         ltft !== null && Math.abs(ltft) >= 10 ? 'warn' : '');

  var fuelText = null;
  if (fuel !== null) {
    fuelText = FUEL_STATUS[fuel] || ('0x' + fuel.toString(16));
  }
  obdSet('fuel', fuelText, fuel !== null && (fuel & 24) ? 'warn' : '');

  if (mil !== null && mil > 0) {
    var codes = dtc === null ? '' : ' \u2014 ' + Math.round(dtc) +
                (dtc == 1 ? ' stored code' : ' stored codes');
    $('#obd-mil').text('\u26a0 check engine light on' + codes).show();
  } else {
    $('#obd-mil').hide();
  }
}

var _wsStaleTimer = null;

function connectWebSocket() {
  var proto = (location.protocol === 'https:') ? 'wss:' : 'ws:';
  var ws = new WebSocket(proto + '//' + location.host + '/ws/carpos');

  function resetStaleTimer() {
    if (_wsStaleTimer) clearTimeout(_wsStaleTimer);
    _wsStaleTimer = setTimeout(function() {
      // No message in 15s — connection is likely dead
      try { ws.close(); } catch(e) {}
    }, 15000);
  }

  ws.onopen = function() {
    resetStaleTimer();
  };

  ws.onmessage = function(event) {
    resetStaleTimer();
    var data = JSON.parse(event.data);
    applyPosition(data);
  };

  ws.onclose = function() {
    if (_wsStaleTimer) { clearTimeout(_wsStaleTimer); _wsStaleTimer = null; }
    setTimeout(connectWebSocket, 2000);
  };

  ws.onerror = function() {
    ws.close();
  };
}

// -- IMU panel --
// The accelerometer reads gravity plus whatever the car is doing, and the
// device's orientation in the car is unknown, so everything is measured
// against a baseline: the accel vector while stationary, which is gravity as
// this device sees it.  Live, the baseline comes from the carpos endpoint
// (mean of recent stationary records) and is then nudged towards each new
// stationary sample; for a replay it is the mean over the journey's
// stationary points.  From there:
//   g force  horizontal dynamic acceleration, i.e. the part of (sample -
//            baseline) perpendicular to gravity: braking, accelerating and
//            cornering.  Coloured by the speed trend, red when slowing.
//   bump     the vertical part of the same, for potholes and speed humps.
//   yaw      gyro rate about the gravity axis, i.e. turning, in deg/s.
//   tilt     angle between the current vector and the baseline, shown only
//            while stationary where it means jacking or towing rather than
//            a corner.
var accelBaseline = null; // {x, y, z} milli-g
var imuPrevSpeed = null;
var BASELINE_EMA = 0.1;

function computeBaseline(points) {
  // average accel values from points where speed < 1 mph (stationary)
  var sx = 0, sy = 0, sz = 0, n = 0;
  for (var i = 0; i < points.length; i++) {
    var p = points[i];
    if (obdNum(p.accel_x) === null) continue;
    if (speedOf(p) < 1) {
      sx += p.accel_x;
      sy += p.accel_y;
      sz += p.accel_z;
      n++;
    }
  }
  if (n > 0) {
    return {x: sx / n, y: sy / n, z: sz / n};
  }
  // fallback: use first point with accel data
  for (var i = 0; i < points.length; i++) {
    if (obdNum(points[i].accel_x) !== null) {
      return {x: points[i].accel_x, y: points[i].accel_y, z: points[i].accel_z};
    }
  }
  return null;
}

function imuSet(key, text, cls) {
  var el = $('.obd-value[data-imu="' + key + '"]');
  el.removeClass('empty warn accel brake');
  if (text === null) {
    el.text('-').addClass('empty');
  } else {
    el.text(text);
    if (cls) el.addClass(cls);
  }
}

function updateImu(data) {
  var ax = obdNum(data['accel_x']);
  var ay = obdNum(data['accel_y']);
  var az = obdNum(data['accel_z']);
  var temp = obdNum(data['imu_temp']);

  if ((ax === null && temp === null) || trackMode) {
    $('#imu').hide();
    imuPrevSpeed = null;
    return;
  }
  $('#imu').show();

  imuSet('temp', temp === null ? null : temp.toFixed(1) + '\u00b0C');

  if (ax === null) {
    imuSet('g', null); imuSet('bump', null); imuSet('yaw', null); imuSet('tilt', null);
    $('#imu-bar-fill').css('width', 0);
    return;
  }

  var speed = speedOf(data);
  var stationary = speed < 1;

  // Live: learn the baseline from stationary samples.  A replay has its
  // own baseline from the whole journey and leaves it alone.
  if (!replayActive && stationary) {
    if (accelBaseline === null) {
      accelBaseline = {x: ax, y: ay, z: az};
    } else {
      accelBaseline.x += (ax - accelBaseline.x) * BASELINE_EMA;
      accelBaseline.y += (ay - accelBaseline.y) * BASELINE_EMA;
      accelBaseline.z += (az - accelBaseline.z) * BASELINE_EMA;
    }
  }

  if (accelBaseline === null) {
    imuSet('g', null); imuSet('bump', null); imuSet('yaw', null); imuSet('tilt', null);
    $('#imu-bar-fill').css('width', 0);
    return;
  }

  var b = accelBaseline;
  var bmag = Math.sqrt(b.x * b.x + b.y * b.y + b.z * b.z) || 1;
  var gx = b.x / bmag, gy = b.y / bmag, gz = b.z / bmag; // unit gravity

  // dynamic acceleration split into vertical (along gravity) and horizontal
  var dx = ax - b.x, dy = ay - b.y, dz = az - b.z;
  var vert = dx * gx + dy * gy + dz * gz;
  var hx = dx - vert * gx, hy = dy - vert * gy, hz = dz - vert * gz;
  var horiz = Math.sqrt(hx * hx + hy * hy + hz * hz);
  var gForce = horiz / 1000;
  var bump = vert / 1000;

  var trend = '';
  if (imuPrevSpeed !== null && gForce >= 0.05) {
    trend = speed < imuPrevSpeed ? 'brake' : 'accel';
  }
  imuPrevSpeed = speed;

  imuSet('g', gForce.toFixed(2) + 'g', gForce >= 0.5 ? 'warn' : trend);
  imuSet('bump', (bump >= 0 ? '+' : '') + bump.toFixed(2) + 'g',
         Math.abs(bump) >= 0.5 ? 'warn' : '');

  var yawRate = null;
  var gxr = obdNum(data['gyro_x']), gyr = obdNum(data['gyro_y']), gzr = obdNum(data['gyro_z']);
  if (gxr !== null) {
    yawRate = gxr * gx + gyr * gy + gzr * gz;
  }
  imuSet('yaw', yawRate === null ? null :
         (yawRate > 0 ? '+' : '') + yawRate.toFixed(1));

  if (stationary) {
    var amag = Math.sqrt(ax * ax + ay * ay + az * az) || 1;
    var cosT = (ax * gx + ay * gy + az * gz) / amag;
    var tilt = Math.acos(Math.max(-1, Math.min(1, cosT))) * 180 / Math.PI;
    imuSet('tilt', tilt.toFixed(1) + '\u00b0', tilt >= 5 ? 'warn' : '');
  } else {
    imuSet('tilt', null);
  }

  $('#imu-bar-fill').css({
    width: (Math.min(gForce / 0.5, 1) * 100) + '%',
    background: trend === 'brake' ? '#e53935' : '#43a047'
  });
}

// -- History popup & journey replay --
// replayIndex is the index of the currently shown frame (not "next to show").
var replayPath = null;
var replayTimer = null;
var replayIndex = 0;
var replayPoints = [];
var replayActive = false;
var replayPlaying = false;
var replayDragging = false;
var replayWasPlayingBeforeDrag = false;
var historyPage = 0;
var historyLoading = false;
var REPLAY_SKIP = 10; // frames to skip per back/forward click

function openHistory() {
  historyPage = 0;
  $('#history-list').empty();
  $('#history-popup').show();
  loadJourneys();
}

function closeHistory() {
  $('#history-popup').hide();
}

function loadJourneys() {
  if (historyLoading) return;
  historyLoading = true;
  $.ajax({
    type: 'GET',
    url: '/api/1.0/journeys?page=' + historyPage,
    dataType: 'json',
    success: function(resp) {
      var data = resp.journeys || [];
      historyLoading = false;
      if (data.length === 0) {
        if (historyPage === 0) {
          $('#history-list').html('<div style="padding:12px;color:#888">No journeys found</div>');
        }
        $('#history-more').hide();
        return;
      }
      $('#history-more').show();
      for (var i = 0; i < data.length; i++) {
        var j = data[i];
        var start = j.start_time;
        var miles = j.miles.toFixed(1);
        var from = j.from_place || '?';
        var to = j.to_place || '?';
        var el = $('<div class="history-item" data-id="' + j.id + '">' +
          start + ' &mdash; ' + miles + ' mi<br>' +
          '<span style="color:#888;font-size:0.9em">' +
          $('<div>').text(from).html() + ' &rarr; ' + $('<div>').text(to).html() +
          '</span></div>');
        $('#history-list').append(el);
      }
    }
  });
}

$(document).on('click', '#history-more', function() {
  historyPage++;
  loadJourneys();
});

$(document).on('click', '.history-item', function() {
  var id = $(this).data('id');
  closeHistory();
  startReplay(id);
});

function startReplay(journeyId) {
  stopReplay();
  replayActive = true;
  $('#replay-controls').show();
  $('#replay-status').text('Loading...');
  $('#replay-progress').attr('max', 0).val(0);
  $.ajax({
    type: 'GET',
    url: '/api/1.0/journey/' + journeyId + '/points',
    dataType: 'json',
    success: function(resp) {
      var data = resp.points || [];
      if (data.length === 0) {
        $('#replay-status').text('No data points');
        return;
      }
      replayPoints = data;
      replayIndex = 0;
      accelBaseline = computeBaseline(data);
      imuPrevSpeed = null;
      $('#replay-progress').attr('max', data.length - 1).val(0);

      // draw the full path
      var pathCoords = [];
      for (var i = 0; i < data.length; i++) {
        pathCoords.push({lat: data[i].latitude, lng: data[i].longitude});
      }
      replayPath = new google.maps.Polyline({
        path: pathCoords,
        geodesic: true,
        strokeColor: '#4285F4',
        strokeOpacity: 0.8,
        strokeWeight: 3,
        map: map
      });

      // fit map to journey bounds
      var bounds = new google.maps.LatLngBounds();
      for (var i = 0; i < pathCoords.length; i++) {
        bounds.extend(pathCoords[i]);
      }
      map.fitBounds(bounds);

      // show first frame and start playing
      showReplayPoint(0);
      replayPlaying = true;
      updatePlayPauseIcon();
      replayTimer = setTimeout(replayStep, 100);
    }
  });
}

function fetchLivePosition() {
  engineRunning = parseInt($('input#engine_running_init').val()) === 1;
  belowVoltageCount = engineRunning ? 0 : parseInt($('input#engine_stopped_count').val());
  $.ajax({
    type: 'GET',
    url: '/api/1.0/carpos',
    dataType: 'json',
    success: function(resp) {
      accelBaseline = resp.accel_baseline || null;
      imuPrevSpeed = null;
      if (resp.track_mode !== undefined) setTrackViewFromServer(parseInt(resp.track_mode) === 1);
      if (resp.position) applyPosition(resp.position);
    }
  });
}

function replayStep() {
  if (!replayActive || !replayPlaying) return;
  if (replayIndex >= replayPoints.length - 1) {
    // reached the end
    replayPlaying = false;
    updatePlayPauseIcon();
    return;
  }
  replayIndex++;
  showReplayPoint(replayIndex);
  replayTimer = setTimeout(replayStep, 100);
}

function showReplayPoint(idx) {
  if (idx < 0 || idx >= replayPoints.length) return;
  var pt = replayPoints[idx];
  applyPosition(pt);
  $('#replay-status').text((idx + 1) + '/' + replayPoints.length +
    ' — ' + speedOf(pt).toFixed(0) + ' mph — ' + pt.timestamp);
  $('#replay-progress').val(idx);
}

function seekReplay(idx) {
  if (!replayActive || replayPoints.length === 0) return;
  idx = Math.max(0, Math.min(replayPoints.length - 1, idx));
  if (replayTimer) {
    clearTimeout(replayTimer);
    replayTimer = null;
  }
  replayIndex = idx;
  showReplayPoint(idx);
  if (replayPlaying && replayIndex < replayPoints.length - 1) {
    replayTimer = setTimeout(replayStep, 100);
  } else if (replayIndex >= replayPoints.length - 1) {
    replayPlaying = false;
    updatePlayPauseIcon();
  }
}

function pauseReplay() {
  replayPlaying = false;
  if (replayTimer) {
    clearTimeout(replayTimer);
    replayTimer = null;
  }
  updatePlayPauseIcon();
}

function resumeReplay() {
  if (!replayActive || replayPoints.length === 0) return;
  // if at end, restart from beginning
  if (replayIndex >= replayPoints.length - 1) {
    replayIndex = 0;
    showReplayPoint(0);
  }
  replayPlaying = true;
  updatePlayPauseIcon();
  replayTimer = setTimeout(replayStep, 100);
}

function togglePlayPause() {
  if (!replayActive) return;
  if (replayPlaying) pauseReplay();
  else resumeReplay();
}

function updatePlayPauseIcon() {
  // pause glyph when playing, play glyph when paused
  $('#replay-playpause').html(replayPlaying ? '&#x23f8;' : '&#x25b6;');
}

function stopReplay() {
  replayActive = false;
  replayPlaying = false;
  if (replayTimer) {
    clearTimeout(replayTimer);
    replayTimer = null;
  }
  if (replayPath) {
    replayPath.setMap(null);
    replayPath = null;
  }
  replayPoints = [];
  replayIndex = 0;
  accelBaseline = null;
  $('#replay-controls').hide();
  fetchLivePosition();
}

$(document).on('click', '#history-link', function(e) {
  e.preventDefault();
  openHistory();
});
$(document).on('click', '#history-close', function() { closeHistory(); });
$(document).on('click', '#replay-stop', function() { stopReplay(); });
$(document).on('click', '#replay-playpause', function() { togglePlayPause(); });
$(document).on('click', '#replay-back', function() { seekReplay(replayIndex - REPLAY_SKIP); });
$(document).on('click', '#replay-forward', function() { seekReplay(replayIndex + REPLAY_SKIP); });

// Slider drag: pause while dragging, resume afterwards if it was playing.
// mouseup/touchend are bound on document so a release outside the slider
// bounds still ends the drag.
$(document).on('mousedown touchstart', '#replay-progress', function() {
  replayDragging = true;
  replayWasPlayingBeforeDrag = replayPlaying;
  if (replayPlaying) pauseReplay();
});
$(document).on('input', '#replay-progress', function() {
  var v = parseInt(this.value, 10);
  if (!isNaN(v)) seekReplay(v);
});
$(document).on('mouseup touchend touchcancel', function() {
  if (!replayDragging) return;
  replayDragging = false;
  if (replayWasPlayingBeforeDrag) resumeReplay();
  replayWasPlayingBeforeDrag = false;
});

function updateMap() {
  fetchLivePosition();
  connectWebSocket();
}


// -- Track mode ---------------------------------------------------------------
// A server-side switch (POST /api/1.0/trackmode) that the device follows: GNSS
// off, a record every ~500 ms with the ECU's fast PIDs and a 26 Hz burst of
// IMU samples.  While it is on the map is replaced by this dashboard.  Rows
// carry track=1 when the device was actually in the mode, so a freshly
// toggled page shows "waiting" until the first such row arrives.
var trackMode = false;        // the switch, as the server has it
var tkRecords = [];           // {t, rpm, thr, g} per track row, last 60 s
var tkSamples = [];           // {t, lon, lat, h} per IMU sample, last ~3 s
var tkArrivals = [];          // arrival times, for the rate figure
var tkLastArrival = 0;
var tkPrevSpeed = null;
var tkForward = null;         // {x, y, z} unit forward axis, learnt (see below)
var TK_REDLINE = 6500;
var TK_RPM_MAX = 7000;
var tkToggledAt = 0;          // when this page last flipped the switch itself

// The switch as reported by the server on rows and pings.  For a few seconds
// after this page toggled it, the local value wins: a row built just before
// the toggle landed can still arrive carrying the old value.
function setTrackViewFromServer(on) {
  if (Date.now() - tkToggledAt < 5000 && on !== trackMode) return;
  setTrackView(on);
}

function setTrackView(on) {
  if (on === trackMode && $('#track').is(':visible') === on) return;
  trackMode = on;
  $('#track-link').toggleClass('on', on).text(on ? 'track \u25cf' : 'track');
  $('body').toggleClass('track-on', on);
  if (on) {
    $('#map').hide();
    $('#obd').hide();
    $('#imu').hide();
    $('#track').show();
  } else {
    $('#track').hide();
    $('#map').show();
    if (map) {
      google.maps.event.trigger(map, 'resize');
      if (marker) map.setCenter(marker.getPosition());
    }
  }
}

$(document).on('click', '#track-link', function(e) {
  e.preventDefault();
  var want = !trackMode;
  if (want && !confirm('Enable track mode? GPS is switched off and the ECU is polled continuously until it is turned off again.')) return;
  $.ajax({
    type: 'POST',
    url: '/api/1.0/trackmode',
    contentType: 'application/json',
    data: JSON.stringify({on: want ? 1 : 0}),
    dataType: 'json',
    success: function(r) { tkToggledAt = Date.now(); setTrackView(parseInt(r.track_mode) === 1); },
    error: function() { alert('could not change track mode'); }
  });
});

try {
  var f = JSON.parse(localStorage.getItem('tkForward'));
  if (f && isFinite(f.x)) tkForward = f;
} catch (e) {}

function tkSet(id, text, cls) {
  var el = $('#' + id);
  el.removeClass('empty warn');
  if (text === null || text === undefined) el.text('-').addClass('empty');
  else { el.text(text); if (cls) el.addClass(cls); }
}

// Split an IMU sample into vertical and horizontal dynamic acceleration
// against the gravity baseline, as updateImu does, then resolve the
// horizontal part into longitudinal and lateral using the learnt forward
// axis.  Returns null without a baseline.
function tkResolve(ax, ay, az) {
  var b = accelBaseline;
  if (!b) return null;
  var bmag = Math.sqrt(b.x * b.x + b.y * b.y + b.z * b.z) || 1;
  var gx = b.x / bmag, gy = b.y / bmag, gz = b.z / bmag;
  var dx = ax - b.x, dy = ay - b.y, dz = az - b.z;
  var vert = dx * gx + dy * gy + dz * gz;
  var hx = dx - vert * gx, hy = dy - vert * gy, hz = dz - vert * gz;
  var r = {hx: hx, hy: hy, hz: hz, h: Math.sqrt(hx * hx + hy * hy + hz * hz) / 1000,
           vert: vert / 1000, lon: null, lat: null};
  if (tkForward) {
    var f = tkForward;
    // lateral axis = gravity x forward, so the pair is right-handed
    var lx = gy * f.z - gz * f.y, ly = gz * f.x - gx * f.z, lz = gx * f.y - gy * f.x;
    r.lon = (hx * f.x + hy * f.y + hz * f.z) / 1000;
    r.lat = (hx * lx + hy * ly + hz * lz) / 1000;
  }
  return r;
}

// The device's orientation in the car is unknown, so the forward axis is
// learnt: when the speed between two records changes by a couple of mph and
// the burst shows a clear horizontal push, that push points along the car's
// axis, backwards when slowing.  Averaged in over a few such events and kept
// in localStorage, since the mounting does not change.
function tkLearnForward(meanH, speed) {
  if (tkPrevSpeed === null || !meanH) { tkPrevSpeed = speed; return; }
  var dv = speed - tkPrevSpeed;
  tkPrevSpeed = speed;
  var mag = Math.sqrt(meanH.hx * meanH.hx + meanH.hy * meanH.hy + meanH.hz * meanH.hz);
  if (Math.abs(dv) < 1.5 || mag < 80) return;
  var sgn = dv > 0 ? 1 : -1;
  var ux = sgn * meanH.hx / mag, uy = sgn * meanH.hy / mag, uz = sgn * meanH.hz / mag;
  if (!tkForward) {
    tkForward = {x: ux, y: uy, z: uz};
  } else {
    tkForward.x += (ux - tkForward.x) * 0.2;
    tkForward.y += (uy - tkForward.y) * 0.2;
    tkForward.z += (uz - tkForward.z) * 0.2;
    var n = Math.sqrt(tkForward.x * tkForward.x + tkForward.y * tkForward.y + tkForward.z * tkForward.z) || 1;
    tkForward.x /= n; tkForward.y /= n; tkForward.z /= n;
  }
  try { localStorage.setItem('tkForward', JSON.stringify(tkForward)); } catch (e) {}
}

function updateTrack(data) {
  var now = Date.now();
  $('#tk-waiting').hide();

  // rate + age
  tkArrivals.push(now);
  while (tkArrivals.length > 20) tkArrivals.shift();
  tkLastArrival = now;
  if (tkArrivals.length >= 2) {
    var span = (tkArrivals[tkArrivals.length - 1] - tkArrivals[0]) / 1000;
    tkSet('tk-rate', span > 0 ? ((tkArrivals.length - 1) / span).toFixed(1) + ' Hz' : null);
  }

  var rpm = obdNum(data['obd_rpm']);
  var speed = speedOf(data);
  var thr = obdNum(data['obd_throttle']);
  var load = obdNum(data['obd_load']);

  tkSet('tk-rpm', rpm === null ? null : Math.round(rpm).toString(), rpm !== null && rpm >= TK_REDLINE ? 'warn' : '');
  $('#tk-rpm-bar').css('width', rpm === null ? 0 : Math.min(rpm / TK_RPM_MAX, 1) * 100 + '%');
  $('#tk-redline').css('left', (TK_REDLINE / TK_RPM_MAX * 100) + '%');
  tkSet('tk-speed', Math.round(speed).toString());
  $('#tk-speed-src').text(obdNum(data['obd_speed']) !== null ? 'ecu' : (obdNum(data['speed']) ? 'gnss' : ''));
  tkSet('tk-throttle', thr === null ? null : thr.toFixed(0) + '%');
  $('#tk-throttle-bar').css('width', thr === null ? 0 : Math.min(thr, 100) + '%');
  tkSet('tk-load', load === null ? null : load.toFixed(0) + '%');
  $('#tk-load-bar').css('width', load === null ? 0 : Math.min(load, 100) + '%');

  // slow tiles: a track record carries one of these per cycle in rotation,
  // so a missing field keeps its previous value rather than blanking
  var slow = {
    'tk-coolant': [obdNum(data['obd_coolant']), function(v) { return Math.round(v) + '\u00b0C'; }, function(v) { return v >= 110; }],
    'tk-intake':  [obdNum(data['obd_intake']),  function(v) { return Math.round(v) + '\u00b0C'; }, null],
    'tk-maf':     [obdNum(data['obd_maf']),     function(v) { return v.toFixed(1); }, null],
    'tk-timing':  [obdNum(data['obd_timing']),  function(v) { return obdSigned(v, 1, '\u00b0'); }, null],
    'tk-stft':    [obdNum(data['obd_stft']),    function(v) { return obdSigned(v, 1, '%'); }, function(v) { return Math.abs(v) >= 10; }],
    'tk-ltft':    [obdNum(data['obd_ltft']),    function(v) { return obdSigned(v, 1, '%'); }, function(v) { return Math.abs(v) >= 10; }],
  };
  for (var id in slow) {
    var v = slow[id][0];
    if (v === null) continue;
    tkSet(id, slow[id][1](v), slow[id][2] && slow[id][2](v) ? 'warn' : '');
  }
  var fuel = obdNum(data['obd_fuel_status']);
  if (fuel !== null) tkSet('tk-fuel', FUEL_STATUS[fuel] || ('0x' + fuel.toString(16)), (fuel & 24) ? 'warn' : '');
  var mil = obdNum(data['obd_mil']);
  if (mil !== null) {
    if (mil > 0) {
      var dtc = obdNum(data['obd_dtc_count']);
      $('#tk-mil').text('\u26a0 check engine light on' + (dtc === null ? '' : ' \u2014 ' + Math.round(dtc) + ' stored')).show();
    } else {
      $('#tk-mil').hide();
    }
  }
  var batt = parseFloat(data['battery_level']);
  tkSet('tk-batt', isNaN(batt) ? null : batt.toFixed(2) + 'v');
  var it = obdNum(data['imu_temp']);
  tkSet('tk-imutemp', it === null ? null : it.toFixed(0) + '\u00b0C');

  // IMU burst: oldest first, 38 ms apart, ending at this record
  var burst = data['imu'] || [];
  var meanH = null, lastR = null, yawSum = 0, yawN = 0;
  if (burst.length && accelBaseline) {
    var b = accelBaseline;
    var bmag = Math.sqrt(b.x * b.x + b.y * b.y + b.z * b.z) || 1;
    var gx = b.x / bmag, gy = b.y / bmag, gz = b.z / bmag;
    meanH = {hx: 0, hy: 0, hz: 0};
    var t0 = now - (burst.length - 1) * 38;
    for (var i = 0; i < burst.length; i++) {
      var smp = burst[i];
      var r = tkResolve(smp[0], smp[1], smp[2]);
      if (!r) continue;
      meanH.hx += r.hx / burst.length; meanH.hy += r.hy / burst.length; meanH.hz += r.hz / burst.length;
      yawSum += smp[3] * gx + smp[4] * gy + smp[5] * gz; yawN++;
      tkSamples.push({t: t0 + i * 38, lon: r.lon, lat: r.lat, h: r.h, hx: r.hx, hy: r.hy, hz: r.hz});
      lastR = r;
    }
  } else if (accelBaseline) {
    lastR = tkResolve(obdNum(data['accel_x']), obdNum(data['accel_y']), obdNum(data['accel_z']));
    if (lastR) tkSamples.push({t: now, lon: lastR.lon, lat: lastR.lat, h: lastR.h, hx: lastR.hx, hy: lastR.hy, hz: lastR.hz});
    var gyr = obdNum(data['gyro_x']);
    if (gyr !== null && accelBaseline) {
      var bb = accelBaseline, bm = Math.sqrt(bb.x * bb.x + bb.y * bb.y + bb.z * bb.z) || 1;
      yawSum = gyr * bb.x / bm + obdNum(data['gyro_y']) * bb.y / bm + obdNum(data['gyro_z']) * bb.z / bm; yawN = 1;
    }
  }
  while (tkSamples.length && tkSamples[0].t < now - 3000) tkSamples.shift();
  tkLearnForward(meanH, speed);
  $('#tk-orient').text(tkForward ? '' : 'orienting\u2026 brake or accelerate');

  var peak = 0;
  for (var k = 0; k < tkSamples.length; k++) if (tkSamples[k].t >= now - 600 && tkSamples[k].h > peak) peak = tkSamples[k].h;
  tkSet('tk-g', lastR ? peak.toFixed(2) + 'g' : null, peak >= 0.8 ? 'warn' : '');
  tkSet('tk-glong', lastR && lastR.lon !== null ? (lastR.lon > 0 ? '+' : '') + lastR.lon.toFixed(2) : null);
  tkSet('tk-glat', lastR && lastR.lat !== null ? (lastR.lat > 0 ? '+' : '') + lastR.lat.toFixed(2) : null);
  tkSet('tk-yaw', yawN ? (yawSum / yawN > 0 ? '+' : '') + (yawSum / yawN).toFixed(1) : null);

  tkRecords.push({t: now, rpm: rpm, thr: thr, g: peak});
  while (tkRecords.length && tkRecords[0].t < now - 60000) tkRecords.shift();

  tkDrawGG();
  tkDrawTrace();
}

// Friction circle: 1 g ring, trail of the last 3 s, the newest sample as a
// dot.  Forward is up once the axis is known; before that the horizontal
// vector is drawn in the sensor's own frame so it still moves.
function tkDrawGG() {
  var c = document.getElementById('tk-gg');
  if (!c) return;
  var ctx = c.getContext('2d');
  var W = c.width, H = c.height, cx = W / 2, cy = H / 2, R = W / 2 - 8, SCALE = R / 1.2;
  ctx.clearRect(0, 0, W, H);
  ctx.strokeStyle = '#333'; ctx.lineWidth = 1;
  [0.5, 1.0].forEach(function(g) { ctx.beginPath(); ctx.arc(cx, cy, g * SCALE, 0, Math.PI * 2); ctx.stroke(); });
  ctx.beginPath(); ctx.moveTo(cx, 4); ctx.lineTo(cx, H - 4); ctx.moveTo(4, cy); ctx.lineTo(W - 4, cy); ctx.stroke();
  ctx.fillStyle = '#666'; ctx.font = '10px courier';
  ctx.fillText('1g', cx + SCALE + 2, cy - 2);
  if (tkForward) { ctx.fillText('accel', cx + 3, 12); ctx.fillText('brake', cx + 3, H - 5); }

  function xy(s) {
    var px, py;
    if (s.lon !== null && s.lat !== null) { px = s.lat; py = -s.lon; }
    else { px = s.hx / 1000; py = s.hy / 1000; }
    return [cx + px * SCALE, cy + py * SCALE];
  }
  var n = tkSamples.length;
  for (var i = 1; i < n; i++) {
    var a = xy(tkSamples[i - 1]), b = xy(tkSamples[i]);
    ctx.strokeStyle = 'rgba(79,195,247,' + (0.15 + 0.85 * i / n).toFixed(2) + ')';
    ctx.lineWidth = 2;
    ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
  }
  if (n) {
    var p = xy(tkSamples[n - 1]);
    ctx.fillStyle = tkSamples[n - 1].h >= 0.8 ? '#e53935' : '#4fc3f7';
    ctx.beginPath(); ctx.arc(p[0], p[1], 5, 0, Math.PI * 2); ctx.fill();
  }
}

// Strip chart of the last 60 s: rpm (amber, 0..TK_RPM_MAX), throttle
// (green, 0..100 %) and peak g per record (blue, 0..1.2 g).
function tkDrawTrace() {
  var c = document.getElementById('tk-trace');
  if (!c) return;
  var ctx = c.getContext('2d');
  var W = c.width, H = c.height, now = Date.now(), T = 60000;
  ctx.clearRect(0, 0, W, H);
  ctx.strokeStyle = '#2a2a2a'; ctx.lineWidth = 1;
  for (var f = 0.25; f < 1; f += 0.25) { ctx.beginPath(); ctx.moveTo(0, H * f); ctx.lineTo(W, H * f); ctx.stroke(); }
  function line(key, max, color) {
    ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.beginPath();
    var started = false;
    for (var i = 0; i < tkRecords.length; i++) {
      var r = tkRecords[i];
      if (r[key] === null || r[key] === undefined) { started = false; continue; }
      var x = W - (now - r.t) / T * W;
      var y = H - Math.min(r[key] / max, 1) * (H - 4) - 2;
      if (!started) { ctx.moveTo(x, y); started = true; } else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }
  line('rpm', TK_RPM_MAX, '#e6a23c');
  line('thr', 100, '#43a047');
  line('g', 1.2, '#4fc3f7');
}

// Age ticks on its own so a stalled feed is visible at a glance.
setInterval(function() {
  if (!trackMode || !tkLastArrival) return;
  var age = (Date.now() - tkLastArrival) / 1000;
  tkSet('tk-age', age < 10 ? age.toFixed(1) + ' s' : Math.round(age) + ' s', age >= 5 ? 'warn' : '');
}, 250);
