/* =====================================================================
   mqtt-live.js — live telemetry straight from HiveMQ over WebSocket.

   Load this BEFORE the dashboard's own <script> block. It takes over as
   the live source (the backend WebSocket is then not opened, so packets
   are never rendered twice) while the backend still serves history
   backfill, playback and CSV export over REST.

   It also owns the two downlink messages:
     · commands        multisense/<dev>/cmd
     · geofence        multisense/<dev>/cfg/geofence   (RETAINED)

   ---------------------------------------------------------------------
   SECURITY — read this before you deploy
   ---------------------------------------------------------------------
   These credentials ship to every visitor in plain text; anyone who opens
   DevTools can read them. So:
     · create a SEPARATE HiveMQ credential for the browser, with
       subscribe-only on multisense/+/telemetry|event|status and publish
       only on multisense/+/cmd and multisense/+/cfg/#
     · never reuse the device's or the backend's credential here
     · if the site is public, set enabled:false and let the backend
       WebSocket be the live source instead — then nothing is exposed.
   ===================================================================== */
window.MqttLive = (function () {

  const MQ = {
    enabled:  true,
    host:     'xxxxxxxxxxxx.s1.eu.hivemq.cloud',   // your cluster host
    port:     8884,                                 // WSS port (8883 is TCP)
    path:     '/mqtt',
    user:     'dashboard-readonly',
    pass:     'CHANGE_ME',
    base:     'multisense',
    deviceId: 'TRK-A7672S-01'                       // the device you command
  };

  let client = null, up = false;

  const T = leaf => `${MQ.base}/${MQ.deviceId}/${leaf}`;

  function enabled() {
    return MQ.enabled && MQ.host && !MQ.host.startsWith('xxxx')
        && typeof mqtt !== 'undefined';
  }

  function start() {
    if (!enabled()) {
      if (MQ.enabled && typeof mqtt === 'undefined')
        toast('MQTT library missing', 'mqtt.min.js did not load — falling back to the backend WebSocket.', 'warn');
      return false;
    }
    const url = `wss://${MQ.host}:${MQ.port}${MQ.path}`;
    client = mqtt.connect(url, {
      username: MQ.user,
      password: MQ.pass,
      clientId: 'dash-' + Math.random().toString(16).slice(2, 10),
      clean: true,
      keepalive: 60,
      reconnectPeriod: 4000,
      connectTimeout: 15000
    });

    client.on('connect', () => {
      up = true;
      client.subscribe([`${MQ.base}/+/telemetry`,
                        `${MQ.base}/+/event`,
                        `${MQ.base}/+/status`], { qos: 1 });
      const src = document.getElementById('logSrc');
      if (src) src.textContent = 'source: live · mqtt ' + MQ.host;
      toast('MQTT connected', 'Live telemetry is streaming from the broker.', 'warn');
    });

    client.on('reconnect', () => { up = false; });
    client.on('close',     () => { up = false; });
    client.on('error', err => {
      up = false;
      toast('MQTT error', String(err && err.message || err), 'err');
    });

    client.on('message', (topic, buf) => {
      let body;
      try { body = JSON.parse(buf.toString()); }
      catch (e) { return; }

      const leaf = topic.split('/').slice(2).join('/');

      if (leaf === 'telemetry') {
        if (demoOn()) return;                   // DEMO switch wins
        // The tracker has no RTC and sends ts:0 — stamp it on arrival, or
        // every point lands in 1970 and the charts collapse.
        if (!body.ts || body.ts < 1e12) body.ts = Date.now();
        applyTelemetry(body);
      } else if (leaf === 'event') {
        const kind = body.type === 'geofence_exit' ? 'err' : 'warn';
        toast('Device event · ' + (body.type || '?'), body.detail || '', kind);
      } else if (leaf === 'status') {
        if (body.state === 'offline')
          toast('Device went offline', 'The broker reported the last-will message — the tracker lost power or coverage.', 'err');
      }
    });
    return true;
  }

  /* called by the dashboard's sendCmd() before it falls back to HTTP */
  function sendCmd(dev, cmd, value) {
    if (!client || !up) return false;
    client.publish(`${MQ.base}/${dev || MQ.deviceId}/cmd`,
                   JSON.stringify({ cmd, value, queued_ts: Date.now() }),
                   { qos: 1 });
    return true;
  }

  /* Retained, so a tracker that boots hours from now still gets the zone.
     gf is the dashboard's object {lat, lng, r}, or null to disarm. */
  function publishFence(gf) {
    if (!client || !up) return false;
    const payload = (gf && gf.r > 0)
      ? { enabled: true, lat: gf.lat, lon: gf.lng, radius_m: gf.r }
      : { enabled: false };
    client.publish(T('cfg/geofence'), JSON.stringify(payload),
                   { qos: 1, retain: true });
    return true;
  }

  return { enabled, start, sendCmd, publishFence, connected: () => up, cfg: MQ };
})();

/* The dashboard calls these two by name — see the patch notes in docs/SETUP.md */
window.mqttSendCmd = (dev, cmd, value) => MqttLive.sendCmd(dev, cmd, value);
window.gfPublish   = gf => MqttLive.publishFence(gf);
