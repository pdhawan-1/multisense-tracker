/* =====================================================================
   MultiSense Tracker Console — DEMO DATA GENERATOR
   ---------------------------------------------------------------------
   Used only while the DEMO DATA switch in the header is ON. Simulates
   the real device end-to-end: GNSS walk, indoor/outdoor phases,
   coloured spectral targets, a VL53L5CX 8×8 depth scene with a moving
   object, battery drain/charge cycles and occasional current-spike
   faults. Safe to delete in production — the dashboard degrades
   gracefully to live-only.

   Exposes:  window.TrackerDemo = { packet(), batteryDemoStep() }
   ===================================================================== */
(function(){
'use strict';

const KEYS = ['f415','f445','f480','f515','f555','f590','f630','f680'];

const demo = {
  lat:30.3540, lon:76.3637, heading:Math.random()*Math.PI*2,
  soc:87, charging:false, t0:Date.now(), phase:'outdoor', phaseT:Date.now(),
  target:{name:'ambient', w:[1,1,1,1,1,1,1,1]}, targetT:0
};

const DEMO_TARGETS = [                                 // reflectance curves per channel
  {name:'ambient', w:[1,1,1,1,1,1,1,1]},
  {name:'red',     w:[0.12,0.10,0.10,0.14,0.22,0.55,1.00,0.95]},
  {name:'green',   w:[0.14,0.20,0.45,1.00,0.90,0.38,0.18,0.14]},
  {name:'blue',    w:[0.85,1.00,0.90,0.35,0.18,0.12,0.10,0.10]},
  {name:'yellow',  w:[0.10,0.12,0.28,0.70,1.00,0.98,0.60,0.30]}
];

/* ---- VL53L5CX scene: background plane + a moving object blob ---- */
function tofScene(now, out){
  const res = 8, grid = new Array(res*res), zst = new Array(res*res);
  const bg = out ? 3300 + Math.sin(now/5200)*450        // open space
                 : 1500 + Math.sin(now/6400)*180;       // room wall
  const ox = 3.5 + 3.0*Math.sin(now/3100);              // object drifts around FoV
  const oy = 3.5 + 2.4*Math.cos(now/4300);
  const od = out ? 900 + 700*Math.abs(Math.sin(now/3600))
                 : 280 + 240*Math.abs(Math.sin(now/2600));
  for (let y = 0; y < res; y++){
    for (let x = 0; x < res; x++){
      const i = y*res + x;
      const r2 = (x-ox)**2 + (y-oy)**2;
      let d = bg + (y - 3.5) * (out ? 55 : 22)          // slight floor/ceiling slope
                 + (Math.random()*60 - 30);
      if (r2 < 4.6) d = od + r2*40 + Math.random()*25;  // the object
      d = Math.round(d);
      if (d > 3950 || (out && Math.random() < 0.02)){   // dropouts at long range
        grid[i] = 0; zst[i] = 255;
      } else {
        grid[i] = d; zst[i] = Math.random() < 0.05 ? 9 : 5;
      }
    }
  }
  const valid = grid.filter((d,i)=>d>0 && zst[i]!==255);
  const min = valid.length ? Math.min(...valid) : 0;
  const max = valid.length ? Math.max(...valid) : 0;
  const cIdx = [27,28,35,36];
  const cVals = cIdx.map(i=>grid[i]).filter(d=>d>0);
  const centre = cVals.length ? Math.round(cVals.reduce((a,b)=>a+b,0)/cVals.length) : 0;
  return { res, grid, zstatus:zst, distance_mm:centre, min_mm:min, max_mm:max,
           signal_mcps: 2 + Math.random()*8,
           ambient_mcps: out ? 1.5 + Math.random() : 0.2 + Math.random()*0.3,
           mode:'8x8 @ 15 Hz' };
}

function demoPacket(){
  const now = Date.now();
  if (now - demo.phaseT > 45000){                       // swap environment every 45 s
    demo.phase = demo.phase === 'outdoor' ? 'indoor' : 'outdoor';
    demo.phaseT = now;
  }
  if (now - demo.targetT > 25000){                      // new colour target every 25 s
    demo.target = DEMO_TARGETS[Math.floor(Math.random()*DEMO_TARGETS.length)];
    demo.targetT = now;
  }
  const out = demo.phase === 'outdoor';

  demo.heading += (Math.random()-.5)*.8;
  const step = out ? 0.000045 : 0.000008;
  demo.lat += Math.cos(demo.heading)*step;
  demo.lon += Math.sin(demo.heading)*step;
  if (demo.charging){
    demo.soc = Math.min(100, demo.soc + 0.18);
    if (demo.soc >= 95) demo.charging = false;
  } else {
    demo.soc = Math.max(5, demo.soc - 0.004);
  }

  const sats = out ? 7 + (Math.random()*4|0) : (Math.random()*4|0);
  const fix  = out ? true : sats >= 4;
  const lux  = out ? 8000 + Math.random()*14000 : 120 + Math.random()*280;
  const base = out ? 900 : 260;
  const spec = {};
  KEYS.forEach((k,i)=>{
    const sun = [0.55,0.7,0.85,1,0.98,0.9,0.8,0.65][i];       // daylight-ish curve
    const led = [0.25,0.9,0.6,0.5,0.75,0.95,0.55,0.3][i];     // white-LED-ish curve
    spec[k] = Math.round(base * (out?sun:led) * demo.target.w[i] * (0.9+Math.random()*0.2));
  });
  const clear = Math.round(Object.values(spec).reduce((a,b)=>a+b,0)*0.9);
  const nir   = Math.round(clear * (out ? 0.26 : 0.10) * (0.9+Math.random()*0.2));

  return {
    ts: now, device_id:'TRK-7070-01', uptime_s:(now-demo.t0)/1000,
    gnss:{ fix, lat:demo.lat, lon:demo.lon, alt_m:249+Math.random()*4,
           speed_kmh: fix ? (out?2.5+Math.random()*2:0.2) : 0,
           sats, hdop: fix ? (out?0.8+Math.random()*0.6:2.5+Math.random()*2) : 9.9 },
    cell:{ mode: out?'LTE-M':'NB-IoT', operator:'Airtel IN', registered:true,
           band: out?'B3':'B8', rssi_dbm: Math.round(out?-64-Math.random()*10:-88-Math.random()*14) },
    tof: tofScene(now, out),
    spectral:{ ...spec, clear, nir, gain:'64x', lux },
    battery:{ voltage_v: 3.3 + demo.soc/100*0.9 + (demo.charging?0.08:0) + (Math.random()-.5)*0.01,
              current_ma: demo.charging ? 420 + Math.random()*80
                        : (Math.random() < 0.006 ? -(1600 + Math.random()*300)   // rare fault spike
                                                 : -(90 + Math.random()*120)),
              soc_pct: demo.soc, charging: demo.charging }
  };
}

function batteryDemoStep(){                 // 1st call → low battery, next calls → toggle charger
  if (!demo.charging && demo.soc > 12) demo.soc = 10.4;
  else demo.charging = !demo.charging;
}

window.TrackerDemo = { packet: demoPacket, batteryDemoStep };
})();
