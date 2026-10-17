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
   This is pointed at EMQX's free PUBLIC broker. There is no account and
   no password, which is why it works immediately — and it also means
   anyone who learns your topic root can read your telemetry and publish
   commands to your device.

   Two mitigations, both of which also live in the firmware:
     · ROOT is a random namespace, not a guessable word
     · SECRET is attached to every command and geofence push; the
       firmware ignores anything without it

   Neither is real security: both strings are visible to anyone who opens
   DevTools on this page. They stop casual interference during bring-up,
   nothing more. Before this device carries real location data, move to an
   authenticated broker, give the browser its own subscribe-mostly
   credential, and regenerate ROOT and SECRET.

   Note the port: 8084 (WSS), not 8083. A page served over HTTPS cannot
   open a plain ws:// socket — the browser blocks it as mixed content.
   ===================================================================== */
window.MqttLive = (function () {

  const MQ = {
    enabled:  true,
    host:     'broker.emqx.io',   // EMQX free public broker, no signup
    port:     8084,               // WSS. 8083 is plain ws and HTTPS blocks it
    path:     '/mqtt',
    user:     '',                 // public broker: no credentials
    pass:     '',
    base:     'ms-89a4f93827c1',  // MUST match TOPIC_ROOT in config.h
    secret:   'RCxM5n3tiwlYoPDZlbMrnhrS',  // MUST match CMD_SECRET in config.h
    deviceId: 'TRK-A7672S-01'     // the device you command
  };

  let client = null, up = false;

  const T = leaf => `${MQ.base}/${MQ.deviceId}/${leaf}`;

  function enabled() {
    return MQ.enabled && MQ.host && !MQ.host.startsWith('xxxx')
        && typeof mqtt !== 'undefined';
  }

  /* Attach the shared secret the firmware demands on every downlink.
     Omitted when blank, so an authenticated broker needs no change here. */
  function signed(body) {
    return MQ.secret ? { ...body, k: MQ.secret } : body;
  }

  function start() {
    if (!enabled()) {
      if (MQ.enabled && typeof mqtt === 'undefined')
        toast('MQTT library missing', 'mqtt.min.js did not load — falling back to the backend WebSocket.', 'warn');
      return false;
    }
    const url = `wss://${MQ.host}:${MQ.port}${MQ.path}`;
    const opts = {
      /* a duplicate client id on a shared public broker gets you kicked
         off repeatedly by whoever else chose it */
      clientId: 'dash-' + Math.random().toString(16).slice(2, 10),
      clean: true,
      keepalive: 60,
      reconnectPeriod: 4000,
      connectTimeout: 15000
    };
    if (MQ.user) { opts.username = MQ.user; opts.password = MQ.pass; }
    client = mqtt.connect(url, opts);

    client.on('connect', () => {
      up = true;
      client.subscribe([`${MQ.base}/+/telemetry`,
                        `${MQ.base}/+/event`,
                        `${MQ.base}/+/status`], { qos: 1 });
      const src = document.getElementById('logSrc');
      if (src) src.textContent = 'source: live · mqtt ' + MQ.host;
      toast('MQTT connected',
            MQ.user ? 'Live telemetry is streaming from the broker.'
                    : 'Streaming from a PUBLIC broker — anyone with your topic can read this. Fine for testing, not for real tracking.',
            'warn');
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
        /* An accident alert has to be impossible to miss, so it is sticky
           and names the coordinates inline — the map trail may not have
           caught up yet when it fires. */
        const t = body.type || '?';
        const where = (body.lat || body.lon)
          ? ` @ ${(+body.lat).toFixed(5)}, ${(+body.lon).toFixed(5)}` : '';
        if (t === 'accident'){
          toast('ACCIDENT SUSPECTED',
                `${body.detail || ''}${where} · ${body.odo_km ?? '?'} km on the clock`,
                'err', true);
          document.body.classList.add('accident');
        } else if (t === 'accident_cleared'){
          document.body.classList.remove('accident');
          toast('Accident alert cleared', body.detail || '', 'warn');
        } else if (t === 'last_gasp'){
          toast('Final position before shutdown', `${body.detail || ''}${where}`, 'err', true);
        } else {
          const kind = (t === 'geofence_exit' || t === 'undervoltage_alert') ? 'err' : 'warn';
          toast('Device event · ' + t, (body.detail || '') + where, kind);
        }
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
                   JSON.stringify(signed({ cmd, value, queued_ts: Date.now() })),
                   { qos: 1 });
    return true;
  }

  /* Retained, so a tracker that boots hours from now still gets the zones.
     Takes the dashboard's zone objects {lat, lng, r, name} — one, an
     array of up to 4, or null/[] to disarm everything.

     The flat lat/lon/radius_m keys are kept alongside the array so a
     device still running the older single-zone firmware reads zone 1
     rather than silently disarming itself. */
  const FENCE_MAX_ZONES = 4;

  function publishFence(gf) {
    if (!client || !up) return false;
    const list = (Array.isArray(gf) ? gf : (gf ? [gf] : []))
      .filter(z => z && z.r > 0)
      .slice(0, FENCE_MAX_ZONES)
      .map((z, i) => ({ name: String(z.name || ('zone ' + (i + 1))).slice(0, 15),
                        lat: z.lat, lon: z.lng ?? z.lon, radius_m: z.r }));

    const payload = list.length
      ? { enabled: true, zones: list,
          lat: list[0].lat, lon: list[0].lon, radius_m: list[0].radius_m }
      : { enabled: false, zones: [] };

    client.publish(T('cfg/geofence'), JSON.stringify(signed(payload)),
                   { qos: 1, retain: true });
    return true;
  }

  return { enabled, start, sendCmd, publishFence, connected: () => up, cfg: MQ };
})();

/* The dashboard calls these two by name — see the patch notes in docs/SETUP.md */
window.mqttSendCmd = (dev, cmd, value) => MqttLive.sendCmd(dev, cmd, value);
window.gfPublish   = gf => MqttLive.publishFence(gf);
