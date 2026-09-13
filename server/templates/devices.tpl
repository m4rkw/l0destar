<!DOCTYPE html>
<html>
  <head>
    <title>Tracking — devices</title>
    <meta name="viewport" content="initial-scale=1.0">
    <meta charset="utf-8">
    <link rel="stylesheet" type="text/css" href="/static/css/style.css" />
    <link rel="apple-touch-icon" href="/static/img/favicon.png">
  </head>
  <body>
    <div class="device-list">
      <p>
        <strong>Devices</strong>
        <span class="links"><a href="/logout">logout</a></span>
      </p>
      {% if devices %}
      <div class="table-scroll">
        <table>
          <tr>
            <th>vehicle</th>
            <th>last seen</th>
            <th>state</th>
            <th>battery</th>
            <th>speed</th>
            <th>firmware</th>
            <th>network</th>
          </tr>
          {% for d in devices %}
          <tr>
            <td>
              <a href="{{ url_for('views.track', imei=d.imei) }}">{{ d.registration or d.name or d.imei }}</a>
              {% if d.registration and d.name %}<br><span class="dim">{{ d.name }}</span>{% endif %}
              <br><span class="dim">{{ d.imei }}</span>
            </td>
            <td>{{ d.age }}{% if d.last_seen %}<br><span class="dim">{{ d.last_seen }}</span>{% endif %}</td>
            <td>{{ d.state }}{% if d.track_mode %}<br><span class="dim">track mode</span>{% endif %}</td>
            <td>{% if d.battery_level is not none %}{{ '%.2f' % d.battery_level }}v{% else %}-{% endif %}</td>
            <td>{% if d.speed is not none %}{{ d.speed | round(0) | int }} mph{% else %}-{% endif %}</td>
            <td>{{ d.fw or '-' }}</td>
            <td>{{ d.operator or '-' }}{% if d.rat %}<br><span class="dim">{{ d.rat }}</span>{% endif %}</td>
          </tr>
          {% endfor %}
        </table>
      </div>
      {% else %}
      <p class="note">No devices are enrolled yet. Enrol one on the server with
        <code>tools/adddevice.py &lt;imei&gt; &lt;name&gt;</code>.</p>
      {% endif %}
    </div>
  </body>
</html>
