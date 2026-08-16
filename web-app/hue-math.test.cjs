
const fs = require('fs');
const src = fs.readFileSync('web-app/js/app.js', 'utf8');
function extract(name) {
  const re = new RegExp('function ' + name + '\\([\\s\\S]*?\\n  \\}', 'm');
  const m = src.match(re);
  if (!m) throw new Error('not found: ' + name);
  return m[0];
}
eval(extract('norm360'));
eval(extract('rgbToHueDeg'));
eval(extract('rgbToHsl'));
eval(extract('hslToRgb'));

let fails = 0;
function approx(a, b, eps, label) {
  const ok = Math.abs(a - b) <= eps;
  if (!ok) { fails++; console.log('FAIL', label, 'got', a, 'want', b); }
  else console.log('ok  ', label, '=', a);
}

approx(rgbToHueDeg(255,0,0), 0, 0.01, 'hue red');
approx(rgbToHueDeg(0,255,0), 120, 0.01, 'hue green');
approx(rgbToHueDeg(0,0,255), 240, 0.01, 'hue blue');
approx(rgbToHueDeg(25,125,235), 211.43, 0.5, 'hue fw-default');
approx(rgbToHueDeg(128,128,128), 0, 0.01, 'hue gray');
approx(norm360(-30), 330, 0.001, 'norm -30');
approx(norm360(390), 30, 0.001, 'norm 390');

// Firmware-default base color: user picks red (target 0) -> shift should be ~148.57
const B = rgbToHueDeg(25,125,235);
const shift = Math.round(norm360(0 - B));
approx(shift, 149, 1, 'shift for red target');
// Echo back: displayed = norm360(S + B) ~ 0 (mod 360)
const disp = norm360(shift + B);
const dispWrapped = Math.min(disp, 360 - disp);
approx(dispWrapped, 0, 1, 'echo round-trip to red');

// Round-trip across the hue circle
for (const t of [0, 30, 60, 120, 180, 240, 280, 320, 359]) {
  const s = Math.round(norm360(t - B));
  const d = norm360(s + B);
  const err = Math.min(Math.abs(d - t), 360 - Math.abs(d - t));
  if (err > 1) { fails++; console.log('FAIL roundtrip target', t, 'displayed', d); }
}
console.log('ok   round-trips 0..359 within 1 deg');

// hslToRgb sanity
const red = hslToRgb(0, 1, 0.5), grn = hslToRgb(120, 1, 0.5), blu = hslToRgb(240, 1, 0.5);
approx(red.r, 255, 1, 'hsl red r'); approx(red.g, 0, 1, 'hsl red g');
approx(grn.g, 255, 1, 'hsl green g'); approx(blu.b, 255, 1, 'hsl blue b');
// base-color badge path: displayed hue = base hue reproduces approx the base rgb
const hsl = rgbToHsl(25, 125, 235);
const back = hslToRgb(B, hsl.s / 100, hsl.l / 100);
approx(back.r, 25, 12, 'badge base r'); approx(back.g, 125, 12, 'badge base g'); approx(back.b, 235, 12, 'badge base b');

console.log(fails === 0 ? 'ALL TESTS PASSED' : fails + ' TESTS FAILED');
process.exit(fails === 0 ? 0 : 1);
