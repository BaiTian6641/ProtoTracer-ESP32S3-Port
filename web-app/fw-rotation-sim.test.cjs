
// Faithful simulation of the FIRMWARE's ProtoRGBColor::HueShift.
// Quaternion::RotateVector normalizes the quaternion first (Quaternion.h line 78),
// so HueShift(deg) is EXACTLY a Rodrigues rotation of the RGB vector about the
// (1,1,1)/sqrt(3) axis by 'deg' degrees, followed by Constrain(0,255) and
// float->uint8 truncation (C++ cast semantics).
function norm360(x){ return ((x % 360) + 360) % 360; }
function hueOf(r,g,b){
  const rn=r/255, gn=g/255, bn=b/255;
  const max=Math.max(rn,gn,bn), min=Math.min(rn,gn,bn), d=max-min;
  if(d===0) return 0;
  let h;
  if(max===rn) h=((gn-bn)/d)%6; else if(max===gn) h=(bn-rn)/d+2; else h=(rn-gn)/d+4;
  return norm360(h*60);
}
function fwHueShift(r,g,b,deg){
  const th = deg*Math.PI/180, c=Math.cos(th), s=Math.sin(th);
  const inv = 1/Math.sqrt(3); const k=[inv,inv,inv];
  const v=[r,g,b];
  const dot = k[0]*v[0]+k[1]*v[1]+k[2]*v[2];
  const cross=[k[1]*v[2]-k[2]*v[1], k[2]*v[0]-k[0]*v[2], k[0]*v[1]-k[1]*v[0]];
  const out = v.map((vi,i)=>{
    let x = vi*c + cross[i]*s + k[i]*dot*(1-c);
    x = Math.min(255, Math.max(0, x));           // Constrain(0,255)
    return x < 0 ? Math.ceil(x) : Math.floor(x); // C++ float->uint8 truncation
  });
  return out;
}
// App formula (extracted verbatim from web-app/js/app.js)
function rgbToHueDeg(r,g,b){ return hueOf(r,g,b); }
function appShift(T, B){ return Math.round(norm360(T - B)); }

// 0) Direction sanity: firmware rotate pure red by +120 must be green (hue 120)
const dir = fwHueShift(255,0,0,120);
console.log('direction check: red +120 ->', dir, 'hue =', hueOf(...dir).toFixed(1), '(expect ~120)');

// 1) Swatch honesty across bases: does tapping swatch T land the device on hue T?
const presets = [0,30,60,120,180,240,280,320];
const bases = [
  ['fw-default user cfg', 25,125,235],
  ['green user cfg',      0,255,  0],
  ['purple user cfg',   128,  0,128],
];
let worst = 0, fails = 0;
for (const [name, r,g,b] of bases){
  const B = rgbToHueDeg(r,g,b);
  const errs = [];
  for (const T of presets){
    const S = appShift(T, B);
    const out = fwHueShift(r,g,b,S);
    const h = hueOf(...out);
    let err = Math.abs(h - T); err = Math.min(err, 360-err);
    errs.push(err);
    if (err > worst) worst = err;
    if (err > 3) { fails++; console.log('  MISS', name, 'target', T, '-> landed', h.toFixed(1), 'err', err.toFixed(1)); }
  }
  console.log(name.padEnd(22), 'base hue', B.toFixed(1).padStart(6), '| worst swatch hue error:', Math.max(...errs).toFixed(2)+'°');
}
console.log(fails===0 ? 'ALL 24 swatch taps land within 3° of the swatch hue (worst '+worst.toFixed(2)+'°)' : fails+' MISSES');

// 2) Real face chain: Default expression = gradientSpectrum, stops = JSON colour (30,180,235)
//    then user RGB (use_user_data), per JsonDrivenProtogenAnimation.h:1015-1030.
//    Show where BOTH stops land when tapping swatches with base from USER CONFIG (25,125,235).
console.log('\nReal gradient (stops 30/180/235 + 25/125/235), app base from user cfg:');
const B = rgbToHueDeg(25,125,235);
for (const T of [0,120,240]){
  const S = appShift(T,B);
  const s1 = hueOf(...fwHueShift(30,180,235,S));
  const s2 = hueOf(...fwHueShift(25,125,235,S));
  const e1 = Math.min(Math.abs(s1-T), 360-Math.abs(s1-T));
  const e2 = Math.min(Math.abs(s2-T), 360-Math.abs(s2-T));
  console.log('  tap '+String(T).padStart(3)+'° -> stop1(JSON colour) '+s1.toFixed(1)+'° (err '+e1.toFixed(1)+'), stop2(user cfg) '+s2.toFixed(1)+'° (err '+e2.toFixed(1)+')');
}
