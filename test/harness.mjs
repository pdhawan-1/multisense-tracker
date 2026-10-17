/* Headless runtime smoke-test for the MultiSense dashboard.
   Catches init-order errors (TDZ etc.) that `node --check` cannot see,
   then verifies demo packets actually update the DOM and that the
   scroll-reveal engine attaches and fires. Run:  node test/harness.mjs  */
import { JSDOM } from 'jsdom';
import { readFileSync } from 'fs';

const html = readFileSync(new URL('../frontend/index.html', import.meta.url), 'utf8');
const demoJs = readFileSync(new URL('../frontend/demo-data.js', import.meta.url), 'utf8');
const mainJs = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]).pop();

const dom = new JSDOM(html.replace(/<script[\s\S]*?<\/script>/g, ''), {
  url: 'file:///tracker/index.html',           // mimic double-clicking the file in Chrome
  pretendToBeVisual: true,
  runScripts: 'outside-only',                  // window.eval executes in the page realm
});
const { window } = dom;
const G = window;

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

/* ---------- execute demo file, then the real main script ---------- */
const fail = (m) => { console.error('FAIL:', m); process.exit(1); };
try { G.eval(demoJs); } catch (e) { fail('demo-data.js threw: ' + e.message); }
if (!G.TrackerDemo) fail('TrackerDemo not exposed');

try { G.eval(mainJs); } catch (e) { fail('main script threw during init: ' + e.stack.split('\n')[0]); }
console.log('OK  main script initialised without errors');

/* ---------- feed packets through the real pipeline ---------- */
try { G.eval('applyTelemetry(TrackerDemo.packet())'); }
catch (e) { fail('applyTelemetry threw: ' + e.stack.split('\n')[0]); }

const $ = id => G.document.getElementById(id);
const assert = (cond, m) => cond ? console.log('OK ', m) : fail(m);

assert($('gLat').textContent !== '—', 'GNSS latitude rendered: ' + $('gLat').textContent);
assert($('hdrPct').textContent.includes('%'), 'battery pill rendered: ' + $('hdrPct').textContent);
assert($('tofZones').textContent.includes('/'), 'multizone stats rendered: ' + $('tofZones').textContent);
assert(ctxFills > 60, 'heatmap drew ' + ctxFills + ' zone fills');
assert($('envTxt').textContent.length > 1, 'environment box: ' + $('envTxt').textContent);
assert($('colName').textContent !== 'Detecting colour…', 'colour detected: ' + $('colName').textContent);
assert($('colCct').textContent.includes('CCT'), 'CCT line: ' + $('colCct').textContent);
assert($('tofStatusTxt').textContent.length > 1, 'tof status: ' + $('tofStatusTxt').textContent);
assert($('pwrFlagTxt').textContent.length > 1, 'power flag: ' + $('pwrFlagTxt').textContent);

/* reveal engine */
assert(ioObserved > 5, 'reveal observer attached to ' + ioObserved + ' elements');
const revealed = G.document.querySelectorAll('.reveal.visible').length;
assert(revealed === ioObserved, `all ${revealed} reveal elements became visible`);

/* charging path */
G.eval('TrackerDemo.batteryDemoStep(); TrackerDemo.batteryDemoStep();'); // low → charging
G.eval('applyTelemetry(TrackerDemo.packet())');
assert($('hdrEta').textContent.includes('to full') || $('hdrEta').textContent.includes('charging'),
       'charging ETA: ' + $('hdrEta').textContent);
assert($('battPill').classList.contains('charging'), 'charging bolt class applied');

/* --- v1.2 feature pack --- */
G.URL.createObjectURL = () => 'blob:test'; G.URL.revokeObjectURL = () => {};
G.eval("addAlert('warn','harness alert','check')");
assert(!$('alertBadge').hidden && $('alertBadge').textContent === '1', 'alert badge shows unread count');
assert($('alertPanel').hidden, 'alert panel starts closed');
$('alertBtn').click();
assert(!$('alertPanel').hidden, 'bell click opens panel');
assert($('alertBadge').hidden, 'opening panel clears unread badge');
$('alertBtn').click();
assert($('alertPanel').hidden, 'second bell click closes panel');
assert($('alertList') !== null, 'alert list present');
assert($('envSub').textContent.match(/STATIONARY|WALKING|VEHICLE/), 'activity in env box: ' + $('envSub').textContent);
assert($('dsDist').textContent.length > 1 && $('dsOut').textContent.length > 0, 'daily summary: ' + $('dsDist').textContent + ' / ' + $('dsOut').textContent);
assert($('bhCap').textContent.includes('mAh'), 'battery health: ' + $('bhCap').textContent);
G.eval('setPlayback(true)');
assert(!$('pbBar').hidden, 'playback bar opens');
G.eval('pbRender(0)');
assert($('pbInfo').textContent.includes('1/'), 'playback scrub renders: ' + $('pbInfo').textContent);
G.eval('setPlayback(false)');
assert($('pbBar').hidden, 'playback bar closes');
const badgeBefore = $('alertBadge').textContent;
$('gfBtn').click();                                   // arm placement
mapHandlers.click({ latlng: { lat: 30.3540, lng: 76.3637 } });  // place zone (also toasts)
G.eval("gfCheck({gnss:{fix:true,lat:30.3540,lon:76.3637}})");   // primes state: inside
G.eval("gfCheck({gnss:{fix:true,lat:31.0,lon:77.0}})");         // exit → alert
const items = G.document.querySelectorAll('.ap-item').length;
assert($('alertBadge').textContent !== badgeBefore && items >= 2,
       'geofence exit raised an alert (badge ' + $('alertBadge').textContent + ', items ' + items + ')');
assert(G.document.querySelectorAll('.ctrl-btn').length === 3, 'device control buttons present');

/* several more packets for stability */
for (let i = 0; i < 10; i++) G.eval('applyTelemetry(TrackerDemo.packet())');
console.log('OK  10 further packets applied cleanly');
console.log('\nALL RUNTIME CHECKS PASSED');

process.exit(0);
