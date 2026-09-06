<html>
  <head>
    <title>Tracking</title>
    <meta name="viewport" content="initial-scale=1.0">
    <meta charset="utf-8">
    <style>
      #map {
        height: 100%;
      }
      html, body {
        height: 100%;
        margin: 0;
        padding: 0;
      }
    </style>
    <script src="/static/js/jquery.min.js" type="text/javascript"></script>
    <link rel="stylesheet" type="text/css" href="/static/css/style.css" />
    <link rel="apple-touch-icon" href="/static/img/favicon.png">
  </head>
  <body>
    <div id="data">
      <input type="hidden" id="engine_running_voltage" value="{{ engine_running_voltage }}" />
      <input type="hidden" id="engine_stopped_count" value="{{ engine_stopped_count }}" />
      <input type="hidden" id="engine_running_init" value="{{ 1 if engine_running else 0 }}" />
      <p>
        <span><strong>{{ registration }}</strong></span>
        <span class="links">
            <a class="gps-link" href="https://maps.google.com/maps/place/{{ log.get('latitude', 0) }},{{ log.get('longitude', 0) }}/" target="_blank">gps</a>
            <a href="#" id="history-link">history</a>
            <a href="/logout">logout</a>
        </span>
        <div class="line">
            <span class="operator">{{ operator }}</span> - 
            <span class="rat">{{ log.get('rat') or '' }}</span>
            <span class="voltage">{{ log.get('battery_level', '-') }}v</span>
        </div>
        <div class="line">
            <span class="date">{{ log.get('display_date', '') }}</span>
        </div>
        <div class="bigline">
            <span class="timestamp">{{ log.get('display_timestamp', '') }}</span>
            <div class="speed">
                <span class="speed">{{ (log.get('combined_speed') or 0) | round(0) | int }}</span> mph
            </div>
            <div class="ignition">{% if engine_running %}engine on{% elif log.get('ignition_state') == 1 %}ignition on{% else %}ignition off{% endif %}</div>
        </div>
      </p>
    </div>
    <div id="map"></div>
    <div id="history-popup" style="display:none">
      <div id="history-panel">
        <div id="history-header">
          <strong>Journey History</strong>
          <span id="history-close">&times;</span>
        </div>
        <div id="history-list"></div>
        <div id="history-more" style="display:none;padding:8px;text-align:center;cursor:pointer;color:#4285F4">Load more</div>
      </div>
    </div>
    <div id="replay-controls" style="display:none">
      <div id="replay-topbar">
        <span id="replay-status"></span>
        <span id="replay-stop" title="Stop">&#x25a0;</span>
      </div>
      <div id="replay-playbar">
        <span id="replay-back" class="replay-btn" title="Back 10s">&#x23ea;</span>
        <span id="replay-playpause" class="replay-btn" title="Play/Pause">&#x23f8;</span>
        <span id="replay-forward" class="replay-btn" title="Forward 10s">&#x23e9;</span>
        <input type="range" id="replay-progress" min="0" max="0" value="0" step="1" />
      </div>
    </div>
    <script src="/static/js/track.js" type="text/javascript"></script>
    {% if google_maps_api_key %}
    <script src="https://maps.googleapis.com/maps/api/js?key={{ google_maps_api_key }}&callback=initMap"
    async defer></script>
    {% else %}
    <script>document.getElementById('map').innerHTML =
      '<p style="padding:1em">Set google_maps_api_key in config.yaml to show the map.</p>';</script>
    {% endif %}
  </body>
</html>
