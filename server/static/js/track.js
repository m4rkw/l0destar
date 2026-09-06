var map = null;
var pos = null;

// The speed to show: the ECU's road speed when the device reported one, else
// GNSS.  The server sends it as combined_speed; points from before that
// column existed only carry the GNSS figure.
function speedOf(d) {
  var v = d['combined_speed'];
  if (v === undefined || v === null || v === '') v = d['speed'];
  return parseFloat(v);
}
var marker = null;
var accuracyCircle = null;
var engineRunning = false;
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
  if (data.ping) return;
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

// -- Acceleration gauge --
var accelBaseline = null; // {x, y, z} gravity baseline from stationary readings

function computeBaseline(points) {
  // average accel values from points where speed < 1 mph (stationary)
  var sx = 0, sy = 0, sz = 0, n = 0;
  for (var i = 0; i < points.length; i++) {
    var p = points[i];
    if (p.accel_x === null) continue;
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
    if (points[i].accel_x !== null) {
      return {x: points[i].accel_x, y: points[i].accel_y, z: points[i].accel_z};
    }
  }
  return null;
}

function updateAccelGauge(pt, prevPt) {
  if (!accelBaseline || pt.accel_x === null) {
    $('#accel-gauge').hide();
    return;
  }
  // dynamic accel = sample minus gravity baseline (X-Y plane only)
  var dx = pt.accel_x - accelBaseline.x;
  var dy = pt.accel_y - accelBaseline.y;
  var mag = Math.sqrt(dx * dx + dy * dy); // milli-g
  var gForce = mag / 1000; // in g

  // determine braking vs acceleration from speed delta
  var braking = false;
  if (prevPt) {
    braking = speedOf(pt) < speedOf(prevPt);
  }

  // scale: 0g = 0px, 0.5g = full width (80px)
  var barWidth = Math.min(gForce / 0.5, 1.0) * 80;
  var color = braking ? '#e53935' : '#43a047';

  $('#accel-gauge').show();
  $('#accel-bar').css({width: barWidth + 'px', background: color});
  $('#accel-value').text(gForce.toFixed(2) + 'g').css('color', color);
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
  $('#accel-gauge').hide();
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
