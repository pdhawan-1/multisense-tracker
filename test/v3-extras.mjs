/* Checks the v3 additions to the dashboard, which the original harness
   knows nothing about: the extras strip and the multi-zone geofence.
   Run: node test/v3-extras.mjs                                        */
import { JSDOM } from 'jsdom';
import { readFileSync } from 'fs';

const html   = readFileSync(new URL('../frontend/index.html', import.meta.url), 'utf8');
const demoJs = readFileSync(new URL('../frontend/demo-data.js', import.meta.url), 'utf8');
const mainJs = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]).pop();

const dom = new JSDOM(html.replace(/<script[\s\S]*?<\/script>/g, ''), {
  url: 'https://tracker.test/', pretendToBeVisual: true, runScripts: 'outside-only',
});
const G = dom.window;

/* ---------- stubs for CDN libraries & browser APIs ---------- */
let ctxFills = 0, ctxStrokes = 0;
const fakeCtx = new Proxy({}, {
  get(t, k) {
    if (k === 'fill')   return () => { ctxFills++; };
    if (k === 'stroke') return () => { ctxStrokes++; };
    return typeof k === 'string' ? () => fakeCtx : undefined;
  },
  set() { return true; },
});
G.HTMLCanvasElement.prototype.getContext = () => fakeCtx;
G.devicePixelRatio = 1;

const chain = () => new Proxy(function(){}, {
  get(t, k) {
    if (k === 'getLatLngs') return () => layerPts;
    if (k === 'setLatLngs') return pts => { layerPts = pts; return chainObj; };
    if (k === 'addLatLng')  return p => { layerPts.push(p); return chainObj; };
    if (k === 'getLatLng')  return () => G.L.latLng(30.354, 76.3637);
    if (k === 'getZoom')    return () => 16;
    return (...a) => chainObj;
  },
  apply: () => chainObj,
});
let layerPts = [];
const chainObj = chain();
const mapHandlers = {};
const mapObj = new Proxy(function(){}, {
  get(t, k) {
    if (k === 'on') return (ev, fn) => { mapHandlers[ev] = fn; return mapObj; };
    if (k === 'getZoom') return () => 16;
    return (...a) => mapObj;
  },
  apply: () => mapObj,
});
G.L = {
  map: () => mapObj, tileLayer: () => chainObj, marker: () => chainObj,
  circle: () => chainObj, polyline: () => chainObj, divIcon: o => o,
  latLng: (a, b) => (Array.isArray(a) ? { lat: a[0], lng: a[1] } : { lat: a, lng: b }),
  latLngBounds: () => ({ pad: () => ({}) }),
  control: { layers: () => chainObj },
};
class FakeChart {
  constructor(el, cfg) { this.data = cfg.data; this.options = cfg.options; }
  update() {}
}
FakeChart.defaults = { font: {}, color: '' };
G.Chart = FakeChart;
let ioObserved = 0;
G.IntersectionObserver = class {
  constructor(cb) { this.cb = cb; }
  observe(el) { ioObserved++; this.cb([{ isIntersecting: true, target: el }], this); }
  unobserve() {}
};
G.fetch = () => Promise.resolve({ ok: false, json: async () => ({}) });
G.WebSocket = class { close(){} };

G.eval(demoJs);
G.eval(mainJs);

const $ = id => G.document.getElementById(id);
let pass = 0;
const ok = (cond, msg) => { if (!cond) { console.error('FAIL ' + msg); process.exit(1); }
                            console.log('OK  ' + msg); pass++; };

/* ---------------- a full v3 packet ---------------- */
const grid = Array.from({ length: 64 }, (_, i) => 1400 + i);
const v3 = {
  ts: Date.now(), device_id: 'TRK-A7672S-01', uptime_s: 4000, fw: '3.0.0', mode: 'normal',
  gnss: { fix: true, src: 'gnss', lat: 30.354, lon: 76.3637, alt_m: 251,
          speed_kmh: 42.5, sats: 11, hdop: 0.9 },
  cell: { mode: 'LTE', operator: 'Airtel IN', rssi_dbm: -68, band: 'EUTRAN-BAND3', registered: true },
  tof: { res: 8, grid, zstatus: new Array(64).fill(5), distance_mm: 1450,
         min_mm: 1400, max_mm: 1463, valid_zones: 64, mode: '8x8 @ 15 Hz' },
  spectral: { f415:210, f445:320, f480:512, f515:640, f555:780, f590:655,
              f630:540, f680:410, clear:3120, nir:460, gain:'64x', lux:512 },
  battery: { voltage_v: 7.82, current_ma: -150, soc_pct: 64, charging: false,
             mah: 1600, capacity_mah: 2500, soc_ocv: 61, soc_src: 'fused',
             resting: false, v_raw: 7.79 },
  trip: { active: true, km: 12.34, max_kmh: 58.2, mins: 27 },
  odo_km: 418.7,
  fence: { armed: 2, outside: 1, zones: [{name:'home',outside:false,r:200},
                                        {name:'office',outside:true,r:300}] },
  accident: false,
  env: { modem_temp_c: 41 },
  diag: { reset_reason: 'power_on', i2c_recoveries: 2, buffered: 5,
          time_synced: true, free_heap: 240000, report_ms: 10000 },
};

G.applyTelemetry(v3);
const grid1 = G.document.getElementById('extrasGrid').textContent;

ok(/418\.7 km/.test(grid1),          'odometer tile rendered');
ok(/12\.34 km/.test(grid1),          'active trip distance rendered');
ok(/27 min/.test(grid1),             'trip duration rendered');
ok(/fused/.test(grid1),              'fuel-gauge method rendered');
ok(/1600\/2500 mAh/.test(grid1),     'coulomb state rendered');
ok(/curve says 61%/.test(grid1),     'OCV cross-check rendered');
ok(/41 °C/.test(grid1),              'modem temperature rendered');
ok(/1\/2 outside/.test(grid1),       'multi-zone fence status rendered');
ok(/home, office/.test(grid1),       'zone names rendered');
ok(/power_on/.test(grid1),           'reset reason rendered');
ok(/10 s/.test(grid1),               'adaptive report rate rendered');
ok(!/undefined|NaN/.test(grid1),     'no undefined or NaN leaked into the strip');

/* ---------------- cell-tower fallback must be labelled, not faked ---------------- */
G.applyTelemetry({ ...v3, gnss: { fix: true, src: 'lbs', lat: 30.35, lon: 76.36,
                                  acc_m: 1400, alt_m: 0, speed_kmh: 0, sats: 0, hdop: 99 } });
const grid2 = G.document.getElementById('extrasGrid').textContent;
ok(/CELL TOWER/.test(grid2),         'LBS position is labelled as a cell-tower estimate');
ok(/±1400 m/.test(grid2),            'LBS accuracy is shown, so it is not read as a precise fix');

/* ---------------- emergency mode is visible ---------------- */
G.applyTelemetry({ ...v3, mode: 'emergency' });
ok(/EMERGENCY/.test(G.document.getElementById('extrasGrid').textContent),
   'emergency mode is surfaced');

/* ---------------- a v2/demo packet must degrade, not explode ---------------- */
G.applyTelemetry(G.TrackerDemo.packet());
const grid3 = G.document.getElementById('extrasGrid').textContent;
ok(!/undefined|NaN/.test(grid3),     'a packet without v3 fields degrades cleanly');

/* ---------------- multi-zone geofence, driven through the real UI ----------
   The page's `gf`/`gfX` are top-level `let` bindings, which a later eval
   cannot reach (it would create a stray global instead). So this clicks
   the actual buttons and fires the actual map handler — which is a better
   test anyway, because it exercises the path a user takes. */
const published = [];
G.window.gfPublish = z => { published.push(JSON.parse(JSON.stringify(z))); return true; };

const clickMap = (lat, lng) => mapHandlers.click({ latlng: G.L.latLng(lat, lng) });

$('gfBtn').onclick();                       // arm placement for zone 1
clickMap(30.3540, 76.3637);                 // place it
ok(published.length >= 1,                   'placing zone 1 published the fence');
let set = published[published.length - 1];
ok(Array.isArray(set),                      'the fence is published as an array of zones');
ok(set.length === 1 && set[0].r > 0,         'zone 1 is in the published set');

$('gfAdd').onclick();                       // "+ ZONE"
clickMap(30.7333, 76.7794);                 // place zone 2
set = published[published.length - 1];
ok(set.length === 2,                         'zone 2 joined the published set');
ok(set[0].name === 'zone 1',                 'zone 1 stays first, as the firmware expects');
ok(set[1].name === 'zone 2',                 'the extra zone is named for the device');
ok(set.every(z => z.r > 0 && z.lat && z.lng), 'every published zone is complete');
ok(/zone 1/.test($('gfZones').textContent) && /zone 2/.test($('gfZones').textContent),
   'both zones are listed in the toolbar: ' + $('gfZones').textContent.trim());

/* the device alarms only when outside EVERY zone, so both must survive a redraw */
$('gfBtn').onclick();                        // clicking GEOFENCE again clears all
set = published[published.length - 1];
ok(Array.isArray(set) && set.length === 0,   'clearing disarms every zone, not just zone 1');

console.log(`\nALL ${pass} v3 CHECKS PASSED`);
/* the page's setInterval timers would otherwise keep Node alive */
dom.window.close();
process.exit(0);
