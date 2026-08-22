// Ask the browser where we are, round that to a tenth of a degree, and offer
// the OSM map data for a box around it.
//
// Three steps, each its own panel: position -> rounded pair -> download. The
// rounded pair names a 0.1° cell, but the download is a much smaller box
// centred on it, capped at 0.01°: the OSM API refuses anything over 50 000
// nodes, and the one route that can serve a whole cell measured 219 MB over
// farmland — not something to hand a phone (see the table in the README).

const $ = (id) => document.getElementById(id);

// ---- geometry ------------------------------------------------------------

// Tenths of a degree, kept as integers. Working in floats here produces
// 52.400000000000006 on screen and in the bbox, and the bbox is a string the
// API parses.
const TENTHS = 10;

// Round half away from zero, so the grid is symmetric across the equator and
// the prime meridian: Math.round(-0.05 * 10) is -0 while Math.round(0.05 * 10)
// is 1, which would place the boundary differently north and south.
function roundTenth(deg) {
  const t = Math.round(Math.abs(deg) * TENTHS) / TENTHS;
  return deg < 0 ? -t : t;
}

// One decimal place, always — "52.4", not "52.40000000000001" and not "52".
function fmtDeg(deg) {
  return deg.toFixed(1);
}

// The cell edges sit on a half-tenth (52.35, 52.45), so they need a second
// decimal — through fmtDeg they collapse into "52.4 to 52.4".
function fmtEdge(deg) {
  return deg.toFixed(2);
}

// Rounding moves the centre by up to 0.05° — about 5.6 km north-south. At the
// sizes this app downloads, the box therefore often does not contain the
// position it came from, which is worth saying out loud rather than leaving to
// be discovered in JOSM.
function offsetKm(pos, cell) {
  const ns = 110.574 * (cell.lat - pos.lat);
  const ew = 111.320 * Math.cos((pos.lat * Math.PI) / 180) * (cell.lon - pos.lon);
  return Math.hypot(ns, ew);
}

// Is the position actually inside the box we are about to ask for?
function boxContains(box, pos) {
  return pos.lat >= box.minLat && pos.lat <= box.maxLat
    && pos.lon >= box.minLon && pos.lon <= box.maxLon;
}

// Latitude is 110.574 km per degree everywhere; longitude shrinks with the
// cosine of the latitude, which is why a cell is taller than it is wide here.
function boxKm(centreLat, size) {
  const ns = 110.574 * size;
  const ew = 111.320 * Math.cos((centreLat * Math.PI) / 180) * size;
  return { ns, ew, km2: ns * ew };
}

// The box to download: `size` degrees across, centred on the rounded pair.
// Clamped to the valid ranges — a cell near a pole or the antimeridian would
// otherwise ask for coordinates that do not exist, and the API answers 400.
function boxAround(lat, lon, size) {
  const half = size / 2;
  const minLat = Math.max(-90, lat - half);
  const maxLat = Math.min(90, lat + half);
  const minLon = Math.max(-180, lon - half);
  const maxLon = Math.min(180, lon + half);
  return { minLat, maxLat, minLon, maxLon };
}

// Six decimals is ~0.1 m — more than the API needs and far more than a phone
// fix is worth, but it keeps the string exact rather than rounded twice.
function fmtBox(box) {
  const n = (v) => v.toFixed(6);
  return `${n(box.minLon)},${n(box.minLat)},${n(box.maxLon)},${n(box.maxLat)}`;
}

// ---- the data source -----------------------------------------------------

// The canonical raw-data call, the same one JOSM makes. Hard limits: 0.25
// square degrees and 50 000 nodes, whichever bites first — in a city the node
// count runs out somewhere between 0.01° and 0.02°.
const OSM_API = 'https://api.openstreetmap.org/api/0.6/map';

// The cell the rounded pair names. Shown, never downloaded: pulling a whole
// 0.1° cell needs Overpass, and one measured over farmland came back as 219 MB
// in 42 seconds. Far too much to hand a phone, so the download is capped well
// below it.
const CELL_SIZE = 0.1;
const MAX_SIZE = 0.01;

function requestUrl(box) {
  return `${OSM_API}?bbox=${fmtBox(box)}`;
}

// The one place the cap is applied. Clamping inside download() alone let the
// panel advertise a box bigger than the one actually fetched — the readouts,
// the filename and the request all have to come from here.
function currentSize() {
  return Math.min(state.size, MAX_SIZE);
}

// ---- formatting ----------------------------------------------------------

function fmtBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} kB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

function fmtKm(km) {
  return km < 1 ? `${Math.round(km * 1000)} m` : `${km.toFixed(1)} km`;
}

// ---- state ---------------------------------------------------------------

const state = {
  position: null,   // { lat, lon, accuracy } as reported, unrounded
  cell: null,       // { lat, lon } rounded to 0.1°
  size: 0.01,       // degrees across, centred on the cell
  busy: false,      // a download is running
};

let controller = null;  // AbortController for the running download

const locateBtn = $('locate-btn');
const locStatus = $('loc-status');
const locNote = $('loc-note');
const locReadout = $('loc-readout');
const cellStatus = $('cell-status');
const cellSpan = $('cell-span');
const sizeRow = $('size-row');
const dlStatus = $('dl-status');
const dlNote = $('dl-note');
const dlBar = $('dl-bar');
const dlBarFill = $('dl-bar-fill');
const downloadBtn = $('download-btn');
const cancelBtn = $('cancel-btn');

function setNote(el, text, kind) {
  el.hidden = !text;
  el.textContent = text || '';
  el.className = kind === 'err' ? 'note err' : 'note';
}

function setPill(el, text, kind) {
  el.className = kind ? `pill ${kind}` : 'pill';
  el.textContent = text;
}

// ---- render --------------------------------------------------------------

function render() {
  const pos = state.position;
  locReadout.hidden = !pos;
  if (pos) {
    $('raw-lat').textContent = `${pos.lat.toFixed(6)} °`;
    $('raw-lon').textContent = `${pos.lon.toFixed(6)} °`;
    $('raw-acc').textContent = pos.accuracy ? `± ${fmtKm(pos.accuracy / 1000)}` : 'unknown';
  }

  const cell = state.cell;
  $('cell-lat').innerHTML = cell ? `${fmtDeg(cell.lat)}<span class="unit"> °${cell.lat < 0 ? 'S' : 'N'}</span>` : '—';
  $('cell-lon').innerHTML = cell ? `${fmtDeg(cell.lon)}<span class="unit"> °${cell.lon < 0 ? 'W' : 'E'}</span>` : '—';

  if (cell) {
    const full = boxAround(cell.lat, cell.lon, CELL_SIZE);
    const km = boxKm(cell.lat, CELL_SIZE);
    setPill(cellStatus, 'rounded', 'ok');
    cellSpan.textContent = `The 0.1° cell runs ${fmtEdge(full.minLat)}° to ${fmtEdge(full.maxLat)}° `
      + `and ${fmtEdge(full.minLon)}° to ${fmtEdge(full.maxLon)}° — about `
      + `${fmtKm(km.ns)} by ${fmtKm(km.ew)}.`;
  }

  // The download panel only means anything once there is a cell to centre on.
  const size = currentSize();
  const box = cell ? boxAround(cell.lat, cell.lon, size) : null;
  const km = cell ? boxKm(cell.lat, size) : null;
  $('dl-area').textContent = km ? `${fmtKm(km.ns)} × ${fmtKm(km.ew)} — ${km.km2.toFixed(1)} km²` : '—';
  $('dl-bbox').textContent = box ? fmtBox(box) : '—';
  $('dl-source').textContent = cell ? 'OpenStreetMap API 0.6' : '—';
  $('dl-centre').textContent = cell && pos
    ? (boxContains(box, pos)
      ? `the rounded pair — you are inside it, ${fmtKm(offsetKm(pos, cell))} from its centre`
      : `the rounded pair — ${fmtKm(offsetKm(pos, cell))} from where you are, so your own position is outside it`)
    : '—';
  downloadBtn.disabled = !cell || state.busy;
  sizeRow.querySelectorAll('input').forEach((input) => { input.disabled = state.busy; });
  cancelBtn.hidden = !state.busy;
}

// ---- step 1: where we are ------------------------------------------------

const GEO_ERRORS = {
  1: 'Permission refused. The browser will not ask again on its own — allow '
    + 'location for this site in the address bar or site settings, then press '
    + 'the button again.',
  2: 'The device could not get a fix. Outdoors or near a window usually helps; '
    + 'on a desktop without GPS this depends on the network.',
  3: 'Timed out waiting for a fix.',
};

function setBusyLocating(busy) {
  locateBtn.disabled = busy;
  locateBtn.textContent = busy ? 'Asking…' : 'Use my location';
}

function locate() {
  // A phone on the LAN over plain http gets no geolocation at all, and the
  // failure is otherwise indistinguishable from a refused permission.
  if (!window.isSecureContext) {
    setPill(locStatus, 'insecure', 'err');
    setNote(locNote, 'This page is not in a secure context, so the browser blocks '
      + 'geolocation. Open it over https://, or on http://localhost. The README '
      + 'has the LAN HTTPS setup for testing on a phone.', 'err');
    return;
  }
  if (!navigator.geolocation) {
    setPill(locStatus, 'unsupported', 'err');
    setNote(locNote, 'This browser has no Geolocation API.', 'err');
    return;
  }

  setBusyLocating(true);
  setPill(locStatus, 'asking…');
  setNote(locNote, '');
  navigator.geolocation.getCurrentPosition(
    (pos) => {
      setBusyLocating(false);
      state.position = {
        lat: pos.coords.latitude,
        lon: pos.coords.longitude,
        accuracy: pos.coords.accuracy,
      };
      state.cell = {
        lat: roundTenth(state.position.lat),
        lon: roundTenth(state.position.lon),
      };
      setPill(locStatus, 'located', 'ok');
      setNote(locNote, '');
      setPill(dlStatus, 'ready');
      render();
    },
    (err) => {
      setBusyLocating(false);
      setPill(locStatus, ['', 'refused', 'unavailable', 'timed out'][err.code] || 'failed', 'err');
      setNote(locNote, GEO_ERRORS[err.code] || err.message || 'Could not get a position.', 'err');
    },
    // A cached fix would round to the same tenth of a degree nine times out of
    // ten, but the point of the app is the reading, so ask for a fresh one.
    { enableHighAccuracy: true, timeout: 15000, maximumAge: 0 }
  );
}

locateBtn.addEventListener('click', locate);

// Ask straight away when the permission is already granted — "first asks your
// location" without a pointless extra tap. Anything else waits for the button,
// because an unprompted request on a fresh visit is what browsers throttle.
async function autoLocate() {
  if (!window.isSecureContext || !navigator.permissions?.query) return;
  try {
    const status = await navigator.permissions.query({ name: 'geolocation' });
    if (status.state === 'granted') locate();
  } catch { /* Safari and older Gecko do not know the geolocation name */ }
}

// ---- step 3: the download ------------------------------------------------

sizeRow.addEventListener('change', (evt) => {
  if (evt.target.name !== 'size') return;
  state.size = Number(evt.target.value);
  setNote(dlNote, '');
  render();
});

// What the server said, translated. The OSM API answers 400 with a sentence of
// plain text, and it is a better message than anything invented here — the
// node limit is the whole reason the size picker exists.
function describeFailure(res, body, size) {
  const text = (body || '').trim().slice(0, 300);
  if (res.status === 400 && /too many nodes/i.test(text)) {
    const smaller = [0.005].find((s) => s < size);
    return `${text}\n\nThat is the API's 50 000-node cap — this spot is dense `
      + `enough that even ${size}° across is too much.`
      + (smaller ? ` Try ${smaller}°.` : '');
  }
  if (res.status === 429 || res.status === 509) {
    return 'The API is rate-limiting or out of bandwidth for now. Wait a minute '
      + 'and try again, or pick a smaller area.';
  }
  if (res.status === 504 || res.status === 502) {
    return 'The API timed out on that query. Retry, or take a smaller box.';
  }
  return `${res.status} ${res.statusText}${text ? `\n\n${text}` : ''}`;
}

// Stream the body so the byte counter moves. Content-Length is usually absent
// on these endpoints (both answer chunked), so the bar only appears when the
// server did say how much is coming.
async function readWithProgress(res) {
  const total = Number(res.headers.get('content-length')) || 0;
  dlBar.hidden = !total;
  dlBarFill.style.width = '0%';

  const reader = res.body.getReader();
  const chunks = [];
  let seen = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    seen += value.length;
    setPill(dlStatus, fmtBytes(seen), 'ok');
    if (total) dlBarFill.style.width = `${Math.min(100, (seen / total) * 100).toFixed(1)}%`;
  }
  return new Blob(chunks, { type: 'application/xml' });
}

// Hand the blob to the browser as a file. The object URL is revoked on a timer
// rather than immediately: revoking it in the same tick cancels the save in
// some browsers before it has read the data.
function saveBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}

function filenameFor(cell, size) {
  const part = (v, pos, neg) => `${Math.abs(v).toFixed(1)}${v < 0 ? neg : pos}`;
  return `osm-${part(cell.lat, 'N', 'S')}-${part(cell.lon, 'E', 'W')}-${size}deg.osm`;
}

async function download() {
  if (!state.cell || state.busy) return;
  const cell = state.cell;
  const size = currentSize();
  const box = boxAround(cell.lat, cell.lon, size);
  const url = requestUrl(box);

  state.busy = true;
  controller = new AbortController();
  setNote(dlNote, '');
  setPill(dlStatus, 'requesting…');
  downloadBtn.textContent = 'Downloading…';
  render();

  try {
    const res = await fetch(url, { signal: controller.signal });
    if (!res.ok) {
      const body = await res.text().catch(() => '');
      setPill(dlStatus, `HTTP ${res.status}`, 'err');
      setNote(dlNote, describeFailure(res, body, size), 'err');
      return;
    }
    const blob = await readWithProgress(res);
    const filename = filenameFor(cell, size);
    saveBlob(blob, filename);
    setPill(dlStatus, fmtBytes(blob.size), 'ok');
    setNote(dlNote, `Saved ${filename} — ${fmtBytes(blob.size)} of OSM XML for `
      + `${fmtBox(box)}.`);
    // Draw the same bytes rather than asking the API twice.
    showMap(await blob.text(), box, state.position);
  } catch (err) {
    if (err.name === 'AbortError') {
      setPill(dlStatus, 'cancelled');
      setNote(dlNote, 'Download cancelled.');
    } else {
      // A cross-origin failure lands here with no status to report, which on a
      // phone is nearly always the connection rather than the API.
      setPill(dlStatus, 'failed', 'err');
      setNote(dlNote, `Could not reach the API: ${err.message || err}`, 'err');
    }
  } finally {
    state.busy = false;
    controller = null;
    downloadBtn.textContent = 'Download map data';
    dlBar.hidden = true;
    render();
  }
}

downloadBtn.addEventListener('click', download);
cancelBtn.addEventListener('click', () => { if (controller) controller.abort(); });

// ---- map render ----------------------------------------------------------
//
// Drawn straight from the XML that was downloaded: parse the nodes and ways,
// project them flat, and paint them on a canvas. No tile service and no
// library — the tiles are somebody else's servers with their own usage policy,
// and the point here is to see the data that was actually fetched.
//
// Relations are not resolved. A multipolygon lake therefore appears as its
// outline rather than a filled shape, which is a fair trade for not
// implementing multipolygon assembly in a prototype.

const mapSection = $('map-section');
const mapCanvas = $('map-canvas');
const mapStatus = $('map-status');
const mapNote = $('map-note');
const ctx = mapCanvas.getContext('2d');

// What was parsed out of the last download, plus the box it was asked for.
let scene = null;   // { ways, count: {nodes, ways}, box, pos }
// Pan and zoom on top of the fit-to-box transform. Reset by "fit".
let view = { scale: 1, tx: 0, ty: 0 };

// Two palettes rather than one dimmed: a map that is legible in light mode is
// muddy in dark, and these are the only colours in the app not driven by the
// CSS variables (canvas cannot read them per shape cheaply).
const PALETTES = {
  light: {
    paper: '#f6f7f9', green: '#d8ecd2', water: '#bcdcf5', building: '#d4d7dd',
    buildingLine: '#b9bec7', major: '#f2b544', majorLine: '#d99a20',
    minor: '#ffffff', minorLine: '#b9bec7', path: '#c98fd6', rail: '#8a8f98',
    boxLine: '#d73a49', ink: '#0f172a', me: '#3b6ef0',
  },
  dark: {
    paper: '#1a1d23', green: '#20361f', water: '#183246', building: '#2f343d',
    buildingLine: '#3d434e', major: '#8a6a1e', majorLine: '#b58a2a',
    minor: '#3a4049', minorLine: '#4a515c', path: '#6b4b78', rail: '#6b7280',
    boxLine: '#ff7b72', ink: '#e6e8eb', me: '#4d7cff',
  },
};

function palette() {
  return window.matchMedia?.('(prefers-color-scheme: dark)').matches ? PALETTES.dark : PALETTES.light;
}

// Which layer a way belongs to, or null to skip it. Order of the tests is the
// drawing priority: a building tagged as landuse is still a building.
function classify(tags) {
  if (tags.building || tags['building:part']) return 'building';
  if (tags.natural === 'water' || tags.landuse === 'reservoir' || tags.waterway === 'riverbank') return 'water';
  if (tags.waterway) return 'stream';
  if (tags.leisure === 'park' || tags.leisure === 'garden' || tags.leisure === 'pitch'
    || tags.natural === 'wood' || tags.natural === 'scrub' || tags.natural === 'grassland'
    || tags.landuse === 'grass' || tags.landuse === 'forest' || tags.landuse === 'meadow') return 'green';
  if (tags.railway) return 'rail';
  const hw = tags.highway;
  if (!hw) return null;
  if (/^(motorway|trunk|primary|secondary|tertiary)(_link)?$/.test(hw)) return 'major';
  if (/^(footway|path|cycleway|steps|track|bridleway|pedestrian)$/.test(hw)) return 'path';
  return 'minor';
}

// Painter's order. Areas first, then the network on top of them.
const LAYERS = ['green', 'water', 'building', 'path', 'rail', 'minor', 'major'];

function parseOsm(xmlText) {
  const doc = new DOMParser().parseFromString(xmlText, 'application/xml');
  if (doc.querySelector('parsererror')) throw new Error('the response was not valid XML');

  const nodes = new Map();
  for (const n of Array.from(doc.getElementsByTagName('node'))) {
    nodes.set(n.getAttribute('id'), [Number(n.getAttribute('lon')), Number(n.getAttribute('lat'))]);
  }

  const ways = [];
  for (const w of Array.from(doc.getElementsByTagName('way'))) {
    const tags = {};
    for (const t of Array.from(w.getElementsByTagName('tag'))) {
      tags[t.getAttribute('k')] = t.getAttribute('v');
    }
    const layer = classify(tags);
    if (!layer) continue;
    const refs = Array.from(w.getElementsByTagName('nd'));
    const pts = [];
    for (const nd of refs) {
      const p = nodes.get(nd.getAttribute('ref'));
      // A way can reference a node outside the bbox that the API did send;
      // one it did not send is simply dropped, leaving a shorter line.
      if (p) pts.push(p);
    }
    if (pts.length < 2) continue;
    const first = pts[0];
    const last = pts[pts.length - 1];
    const closed = pts.length > 2 && first[0] === last[0] && first[1] === last[1];
    ways.push({ layer, pts, closed });
  }
  return { ways, count: { nodes: nodes.size, ways: ways.length } };
}

// The API answers with whole ways, so the data reaches past the bbox on every
// side — and a motorway that carries on for kilometres reaches a very long way
// past it. Framing the data extent therefore shrinks the interesting part to
// nothing; frame the requested box with a little air around it instead and let
// the surplus run off the edges of the canvas.
const FRAME_MARGIN = 0.12;

function framed(box) {
  const dLat = (box.maxLat - box.minLat) * FRAME_MARGIN;
  const dLon = (box.maxLon - box.minLon) * FRAME_MARGIN;
  return {
    minLat: box.minLat - dLat, maxLat: box.maxLat + dLat,
    minLon: box.minLon - dLon, maxLon: box.maxLon + dLon,
  };
}

// Equirectangular, scaled by the cosine of the centre latitude. Over a box a
// kilometre across the difference from Mercator is far under one pixel, and
// this keeps the arithmetic something you can read.
function projector(box) {
  const midLat = (box.minLat + box.maxLat) / 2;
  const kx = Math.cos((midLat * Math.PI) / 180);
  return {
    x: (lon) => (lon - box.minLon) * kx,
    y: (lat) => (box.maxLat - lat),
    w: (box.maxLon - box.minLon) * kx,
    h: box.maxLat - box.minLat,
  };
}

// Fit the requested box into the canvas with a small margin, then apply the
// user's pan and zoom on top.
function transformFor(proj, cssW, cssH) {
  const pad = 10;
  const base = Math.min((cssW - pad * 2) / proj.w, (cssH - pad * 2) / proj.h);
  const scale = base * view.scale;
  return {
    scale,
    ox: (cssW - proj.w * scale) / 2 + view.tx,
    oy: (cssH - proj.h * scale) / 2 + view.ty,
  };
}

function drawScaleBar(c, cssW, cssH, metresPerPx, col) {
  // Pick a round distance that lands between a fifth and a third of the width.
  const target = (cssW / 4) * metresPerPx;
  const nice = [10, 20, 50, 100, 200, 500, 1000, 2000, 5000];
  const metres = nice.find((m) => m >= target) || nice[nice.length - 1];
  const px = metres / metresPerPx;
  const x = 12;
  const y = cssH - 14;
  c.save();
  c.strokeStyle = col.ink;
  c.fillStyle = col.ink;
  c.globalAlpha = 0.75;
  c.lineWidth = 2;
  c.beginPath();
  c.moveTo(x, y - 5); c.lineTo(x, y); c.lineTo(x + px, y); c.lineTo(x + px, y - 5);
  c.stroke();
  c.font = '11px ui-monospace, monospace';
  c.textBaseline = 'bottom';
  c.fillText(metres >= 1000 ? `${metres / 1000} km` : `${metres} m`, x, y - 4);
  c.restore();
}

function drawMap() {
  if (!scene) return;
  const col = palette();
  const rect = mapCanvas.getBoundingClientRect();
  const cssW = rect.width || 320;
  const cssH = rect.height || 240;
  const dpr = window.devicePixelRatio || 1;
  if (mapCanvas.width !== Math.round(cssW * dpr) || mapCanvas.height !== Math.round(cssH * dpr)) {
    mapCanvas.width = Math.round(cssW * dpr);
    mapCanvas.height = Math.round(cssH * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.fillStyle = col.paper;
  ctx.fillRect(0, 0, cssW, cssH);

  const proj = projector(scene.frame);
  const { scale, ox, oy } = transformFor(proj, cssW, cssH);
  const px = (lon) => ox + proj.x(lon) * scale;
  const py = (lat) => oy + proj.y(lat) * scale;

  // Line widths are in metres of ground, so zooming in widens the roads the
  // way a map expects rather than leaving hairlines.
  const metresPerDeg = 110574;
  const mPerPx = 1 / (scale / metresPerDeg);
  const w = (metres, min) => Math.max(min, metres / mPerPx);

  const STROKE = {
    major: () => ({ line: col.majorLine, fill: col.major, casing: w(14, 2.5), core: w(10, 1.5) }),
    minor: () => ({ line: col.minorLine, fill: col.minor, casing: w(8, 1.6), core: w(5.5, 0.9) }),
    path: () => ({ line: col.path, fill: null, casing: 0, core: w(1.5, 0.8), dash: [3, 3] }),
    rail: () => ({ line: col.rail, fill: null, casing: 0, core: w(2, 1), dash: [6, 4] }),
    stream: () => ({ line: col.water, fill: null, casing: 0, core: w(3, 1) }),
  };

  const trace = (way) => {
    ctx.beginPath();
    ctx.moveTo(px(way.pts[0][0]), py(way.pts[0][1]));
    for (let i = 1; i < way.pts.length; i++) ctx.lineTo(px(way.pts[i][0]), py(way.pts[i][1]));
  };

  ctx.lineJoin = 'round';
  ctx.lineCap = 'round';

  for (const layer of LAYERS) {
    const ways = scene.ways.filter((way) => way.layer === layer);
    if (!ways.length) continue;

    if (layer === 'green' || layer === 'water' || layer === 'building') {
      ctx.fillStyle = layer === 'green' ? col.green : layer === 'water' ? col.water : col.building;
      ctx.strokeStyle = layer === 'building' ? col.buildingLine : ctx.fillStyle;
      ctx.lineWidth = 1;
      for (const way of ways) {
        trace(way);
        if (way.closed) { ctx.fill(); if (layer === 'building' && scale > 2e5) ctx.stroke(); }
        else ctx.stroke();
      }
      continue;
    }

    const st = STROKE[layer]();
    ctx.setLineDash(st.dash || []);
    if (st.casing) {
      ctx.strokeStyle = st.line;
      ctx.lineWidth = st.casing;
      for (const way of ways) { trace(way); ctx.stroke(); }
    }
    ctx.strokeStyle = st.fill || st.line;
    ctx.lineWidth = st.core;
    for (const way of ways) { trace(way); ctx.stroke(); }
    ctx.setLineDash([]);
  }

  // The requested box, so it is obvious how much was asked for and where the
  // data stops.
  ctx.save();
  ctx.strokeStyle = col.boxLine;
  ctx.setLineDash([5, 4]);
  ctx.lineWidth = 1.5;
  const bx0 = px(scene.box.minLon);
  const bx1 = px(scene.box.maxLon);
  const by0 = py(scene.box.maxLat);
  const by1 = py(scene.box.minLat);
  ctx.strokeRect(bx0, by0, bx1 - bx0, by1 - by0);
  ctx.restore();

  // Where you actually are, when that is inside the frame at all.
  if (scene.pos) {
    const x = px(scene.pos.lon);
    const y = py(scene.pos.lat);
    ctx.save();
    ctx.fillStyle = col.me;
    ctx.strokeStyle = col.paper;
    ctx.lineWidth = 2.5;
    ctx.beginPath();
    ctx.arc(x, y, 6, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();
    ctx.restore();
  }

  drawScaleBar(ctx, cssW, cssH, mPerPx, col);
}

// Show what was downloaded. Parsing a dense extract takes a moment, so the
// pill says so before the main thread disappears into DOMParser.
function showMap(xmlText, box, pos) {
  mapSection.hidden = false;
  setPill(mapStatus, 'rendering…');
  // Let the pill paint before the parse blocks the thread.
  requestAnimationFrame(() => {
    try {
      const parsed = parseOsm(xmlText);
      const frame = framed(box);
      scene = {
        ways: parsed.ways, count: parsed.count, box, frame,
        pos: pos && boxContains(frame, pos) ? pos : null,
      };
      view = { scale: 1, tx: 0, ty: 0 };
      drawMap();
      setPill(mapStatus, `${parsed.count.nodes} nodes · ${parsed.count.ways} ways`, 'ok');
      mapNote.textContent = 'Drag to pan, pinch or scroll to zoom. The dashed rectangle is the '
        + 'box that was requested — the data reaches past it because the API returns whole ways. '
        + (scene.pos ? 'The dot is where you are.' : 'Your own position is off this map entirely.');
    } catch (err) {
      setPill(mapStatus, 'unreadable', 'err');
      mapNote.textContent = `Could not draw the data: ${err.message || err}`;
    }
  });
}

// ---- map interaction -----------------------------------------------------

function zoomBy(factor, cx, cy) {
  const rect = mapCanvas.getBoundingClientRect();
  const x = cx === undefined ? rect.width / 2 : cx;
  const y = cy === undefined ? rect.height / 2 : cy;
  // Keep the point under the cursor still while the scale changes.
  const next = Math.min(64, Math.max(1, view.scale * factor));
  const applied = next / view.scale;
  view.tx = x - (x - view.tx) * applied;
  view.ty = y - (y - view.ty) * applied;
  view.scale = next;
  drawMap();
}

$('map-in').addEventListener('click', () => zoomBy(1.5));
$('map-out').addEventListener('click', () => zoomBy(1 / 1.5));
$('map-fit').addEventListener('click', () => { view = { scale: 1, tx: 0, ty: 0 }; drawMap(); });

mapCanvas.addEventListener('wheel', (evt) => {
  if (!scene) return;
  evt.preventDefault();
  const rect = mapCanvas.getBoundingClientRect();
  zoomBy(evt.deltaY < 0 ? 1.15 : 1 / 1.15, evt.clientX - rect.left, evt.clientY - rect.top);
}, { passive: false });

// One pointer pans, two pinch. Pointer events cover mouse and touch with the
// same code, which is the whole reason to use them over touch events.
const pointers = new Map();
let pinchStart = null;

mapCanvas.addEventListener('pointerdown', (evt) => {
  if (!scene) return;
  mapCanvas.setPointerCapture(evt.pointerId);
  pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });
  pinchStart = null;
});

mapCanvas.addEventListener('pointermove', (evt) => {
  if (!scene || !pointers.has(evt.pointerId)) return;
  const prev = pointers.get(evt.pointerId);
  pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });

  if (pointers.size === 1) {
    view.tx += evt.clientX - prev.x;
    view.ty += evt.clientY - prev.y;
    drawMap();
    return;
  }
  if (pointers.size === 2) {
    const [a, b] = Array.from(pointers.values());
    const dist = Math.hypot(a.x - b.x, a.y - b.y);
    const rect = mapCanvas.getBoundingClientRect();
    const cx = (a.x + b.x) / 2 - rect.left;
    const cy = (a.y + b.y) / 2 - rect.top;
    if (pinchStart) zoomBy(dist / pinchStart, cx, cy);
    pinchStart = dist;
  }
});

for (const type of ['pointerup', 'pointercancel', 'pointerleave']) {
  mapCanvas.addEventListener(type, (evt) => {
    pointers.delete(evt.pointerId);
    if (pointers.size < 2) pinchStart = null;
  });
}

// Keep the drawing sharp when the window changes, and follow a theme switch.
window.addEventListener('resize', () => drawMap());
window.matchMedia?.('(prefers-color-scheme: dark)').addEventListener?.('change', () => drawMap());

// ---- boot ----------------------------------------------------------------

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').catch(() => {});
  });
}

render();
autoLocate();
