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

// ---- boot ----------------------------------------------------------------

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('./sw.js').catch(() => {});
  });
}

render();
autoLocate();
