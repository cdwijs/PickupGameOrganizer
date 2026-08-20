// QR scanner + generator prototype.
//
// Two decode paths, picked at startup:
//   1. BarcodeDetector  — native, no download, Chrome/Edge/Android.
//   2. jsQR (vendored)  — everywhere else, notably iOS Safari and Firefox.
//
// Whatever ends up in the content textbox — scanned or typed — is re-encoded
// into a QR code by the vendored qrcode-generator, so the app round-trips.
//
// Everything runs from local files so the app also works offline once the
// service worker has cached the shell.

const $ = (id) => document.getElementById(id);
const els = {
  viewport: $('viewport'), video: $('video'), canvas: $('canvas'),
  start: $('btn-start'), stop: $('btn-stop'), switch: $('btn-switch'),
  cameraSelect: $('camera-select'),
  engine: $('engine'), status: $('status'),
  result: $('result'), resultState: $('result-state'), resultMeta: $('result-meta'),
  copy: $('btn-copy'), clear: $('btn-clear'),
  qrCanvas: $('qr-canvas'), qrPlaceholder: $('qr-placeholder'),
  qrState: $('qr-state'), qrMeta: $('qr-meta'), download: $('btn-download'),
};

// qrcode-generator's default string→bytes conversion is latin1 (`charCodeAt &
// 0xff`), which silently mangles anything non-ASCII. Byte mode with real UTF-8
// is what scanners expect, and TextEncoder also handles surrogate pairs.
if (typeof qrcode === 'function') {
  qrcode.stringToBytes = (s) => Array.from(new TextEncoder().encode(s));
}

// Longest edge of the buffer we hand to jsQR. Full-resolution frames make the
// decoder crawl on phones; 640 px is plenty for a QR code that fills the
// reticle. Ignored on the BarcodeDetector path, which reads the video directly.
const DECODE_MAX_EDGE = 640;
const SCAN_INTERVAL_MS = 100;

let stream = null;         // active MediaStream, null when stopped
let detector = null;       // BarcodeDetector instance, null on the jsQR path
let rafId = null;
let lastScanAt = 0;
let decodeBusy = false;    // BarcodeDetector.detect() is async — don't overlap
let lastText = null;
let cameras = [];          // videoinput MediaDeviceInfo[]
let currentDeviceId = null;

function setStatus(msg, kind) {
  els.status.textContent = msg;
  els.status.style.color = kind === 'err' ? 'var(--err)'
    : kind === 'ok' ? 'var(--ok)' : 'var(--muted)';
}

function setPill(el, text, kind) {
  el.textContent = text;
  el.className = 'pill' + (kind ? ' ' + kind : '');
}

// ---------------------------------------------------------------
// Engine selection
// ---------------------------------------------------------------
async function pickEngine() {
  if ('BarcodeDetector' in window) {
    try {
      const formats = await window.BarcodeDetector.getSupportedFormats();
      if (formats.includes('qr_code')) {
        detector = new window.BarcodeDetector({ formats: ['qr_code'] });
        setPill(els.engine, 'engine: BarcodeDetector', 'ok');
        return;
      }
    } catch {
      // Present but unusable (some Linux builds) — fall through to jsQR.
    }
  }
  if (typeof jsQR !== 'function') {
    setPill(els.engine, 'engine: none', 'err');
    throw new Error('No QR decoder available: BarcodeDetector is missing and vendor/jsQR.min.js failed to load.');
  }
  setPill(els.engine, 'engine: jsQR', 'ok');
}

// ---------------------------------------------------------------
// Camera plumbing
// ---------------------------------------------------------------
async function refreshCameraList() {
  // Device labels are only exposed after permission has been granted, so this
  // is called *after* the first successful getUserMedia().
  try {
    const devices = await navigator.mediaDevices.enumerateDevices();
    cameras = devices.filter((d) => d.kind === 'videoinput');
  } catch {
    cameras = [];
  }

  els.cameraSelect.innerHTML = '';
  if (!cameras.length) {
    els.cameraSelect.disabled = true;
    els.cameraSelect.append(new Option('— no camera found —', ''));
    currentDeviceId = null;
    els.switch.disabled = true;
    return;
  }
  cameras.forEach((cam, i) => {
    const opt = new Option(cam.label || `Camera ${i + 1}`, cam.deviceId);
    els.cameraSelect.append(opt);
  });
  els.cameraSelect.value = currentDeviceId || cameras[0].deviceId;
  els.cameraSelect.disabled = false;
  els.switch.disabled = cameras.length < 2;
}

async function start(deviceId) {
  if (!window.isSecureContext) {
    setStatus('Camera access needs a secure context — open this page over https:// or on http://localhost.', 'err');
    return;
  }
  if (!navigator.mediaDevices?.getUserMedia) {
    setStatus('This browser does not expose navigator.mediaDevices.getUserMedia.', 'err');
    return;
  }

  stopStream();
  els.start.disabled = true;
  setStatus('Requesting camera access…');

  // Ask for the rear camera on phones; deviceId wins once the user has picked
  // a specific camera from the dropdown.
  const video = deviceId
    ? { deviceId: { exact: deviceId }, width: { ideal: 1280 }, height: { ideal: 720 } }
    : { facingMode: { ideal: 'environment' }, width: { ideal: 1280 }, height: { ideal: 720 } };

  try {
    stream = await navigator.mediaDevices.getUserMedia({ video, audio: false });
  } catch (err) {
    els.start.disabled = false;
    const name = err?.name || 'Error';
    if (name === 'NotAllowedError' || name === 'SecurityError') {
      setStatus('Camera permission denied. Allow it in the browser’s site settings and try again.', 'err');
    } else if (name === 'NotFoundError' || name === 'OverconstrainedError') {
      setStatus('No usable camera found for the requested settings.', 'err');
    } else if (name === 'NotReadableError') {
      setStatus('The camera is in use by another application.', 'err');
    } else {
      setStatus(`Could not start the camera: ${name} — ${err?.message || err}`, 'err');
    }
    return;
  }

  currentDeviceId = stream.getVideoTracks()[0]?.getSettings?.().deviceId || deviceId || null;
  els.video.srcObject = stream;
  try {
    await els.video.play();
  } catch {
    // Autoplay rejection: the loop below still works once frames arrive.
  }

  els.stop.disabled = false;
  els.viewport.classList.add('scanning');
  await refreshCameraList();

  const track = stream.getVideoTracks()[0];
  const s = track?.getSettings?.() || {};
  setStatus(`Scanning… ${s.width || '?'}×${s.height || '?'}${track?.label ? ' — ' + track.label : ''}`, 'ok');

  lastScanAt = 0;
  rafId = requestAnimationFrame(tick);
}

function stopStream() {
  if (rafId !== null) { cancelAnimationFrame(rafId); rafId = null; }
  if (stream) { stream.getTracks().forEach((t) => t.stop()); stream = null; }
  els.video.srcObject = null;
  els.viewport.classList.remove('scanning');
  els.start.disabled = false;
  els.stop.disabled = true;
  decodeBusy = false;
}

function stop() {
  stopStream();
  setStatus('Stopped.');
}

// ---------------------------------------------------------------
// Scan loop
// ---------------------------------------------------------------
function tick(now) {
  rafId = requestAnimationFrame(tick);
  if (!stream || decodeBusy) return;
  if (now - lastScanAt < SCAN_INTERVAL_MS) return;
  lastScanAt = now;

  // readyState < HAVE_CURRENT_DATA means there is no frame to read yet.
  if (els.video.readyState < 2 || !els.video.videoWidth) return;

  decodeBusy = true;
  decodeFrame()
    .then((text) => { if (text) onDecoded(text); })
    .catch((err) => setStatus(`Decode error: ${err?.message || err}`, 'err'))
    .finally(() => { decodeBusy = false; });
}

async function decodeFrame() {
  if (detector) {
    const codes = await detector.detect(els.video);
    return codes.length ? codes[0].rawValue : null;
  }

  const { videoWidth: vw, videoHeight: vh } = els.video;
  const scale = Math.min(1, DECODE_MAX_EDGE / Math.max(vw, vh));
  const w = Math.round(vw * scale);
  const h = Math.round(vh * scale);
  const canvas = els.canvas;
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }

  const ctx = canvas.getContext('2d', { willReadFrequently: true });
  ctx.drawImage(els.video, 0, 0, w, h);
  const { data } = ctx.getImageData(0, 0, w, h);
  // attemptBoth also catches light-on-dark codes; the cost is one extra pass.
  const code = jsQR(data, w, h, { inversionAttempts: 'attemptBoth' });
  return code?.data || null;
}

function onDecoded(text) {
  if (text === lastText) {
    // Same code still in frame — refresh the timestamp, don't re-notify. This
    // is also what stops the scanner from stomping on a manual edit of a code
    // that happens to still be in view.
    els.resultMeta.textContent = describe(text, 'last seen');
    return;
  }

  lastText = text;
  setContent(text, 'scanned');
  navigator.vibrate?.(60);
}

// ---------------------------------------------------------------
// Content <-> QR code
// ---------------------------------------------------------------
const ECC_LEVEL = 'M';       // ~15% recovery — the usual default
const QUIET_MODULES = 4;     // quiet zone the QR spec requires around the code
const QR_MAX_PX = 320;       // upper bound on the rendered size, in CSS pixels
const QR_MIN_PX = 120;       // below this a dense code stops being scannable

// CSS pixels available for the code: the wrapper's content box, capped. Measured
// per render rather than hardcoded, so a narrow phone gets a smaller code
// instead of a page that scrolls sideways.
function qrTargetPx() {
  const wrap = els.qrCanvas.parentElement;
  const cs = getComputedStyle(wrap);
  const inner = wrap.clientWidth - parseFloat(cs.paddingLeft) - parseFloat(cs.paddingRight);
  return Math.max(QR_MIN_PX, Math.min(QR_MAX_PX, Math.floor(inner)));
}

const byteLength = (s) => new TextEncoder().encode(s).length;

function describe(text, verb) {
  return `${text.length} characters · ${byteLength(text)} bytes · ${verb} ${new Date().toLocaleTimeString()}`;
}

// Single path for both origins: a scan and a keystroke land in the same place.
function setContent(text, origin) {
  els.result.value = text;
  updateContentMeta(text, origin);
  renderQrCode(text);
}

function updateContentMeta(text, origin) {
  els.copy.disabled = !text;
  els.clear.disabled = !text;
  if (!text) {
    setPill(els.resultState, 'nothing scanned');
    els.resultMeta.textContent = '';
    return;
  }
  const verb = origin === 'scanned' ? 'scanned' : 'edited';
  setPill(els.resultState, verb, 'ok');
  els.resultMeta.textContent = describe(text, verb);
}

function renderQrCode(text) {
  const canvas = els.qrCanvas;

  if (!text) {
    canvas.classList.remove('ready');
    canvas.width = canvas.height = 0;
    els.qrPlaceholder.classList.remove('hidden');
    els.qrPlaceholder.textContent = 'Nothing to encode yet.';
    setPill(els.qrState, 'empty');
    els.qrMeta.textContent = '';
    els.download.disabled = true;
    return;
  }

  let qr;
  try {
    qr = qrcode(0, ECC_LEVEL);   // type 0 = smallest version the data fits in
    qr.addData(text);            // byte mode, UTF-8 via the override above
    qr.make();
  } catch (err) {
    // Thrown when the payload exceeds even a version-40 code. Note that
    // qrcode-generator throws bare strings, not Error objects.
    const reason = err?.message || String(err);
    canvas.classList.remove('ready');
    els.qrPlaceholder.classList.remove('hidden');
    els.qrPlaceholder.textContent = 'Too long to encode as a single QR code.';
    setPill(els.qrState, 'too long', 'err');
    els.qrMeta.textContent =
      `${byteLength(text)} bytes — a version-40 code at level ${ECC_LEVEL} holds about 2331. ${reason}`;
    els.download.disabled = true;
    return;
  }

  const n = qr.getModuleCount();
  const total = n + QUIET_MODULES * 2;
  const dpr = window.devicePixelRatio || 1;
  // Whole device pixels per module, so no module edge lands on a half pixel.
  const scale = Math.max(2, Math.floor((qrTargetPx() * dpr) / total));
  const px = total * scale;

  canvas.width = px;
  canvas.height = px;
  // Rounding up would exceed the measured space and reintroduce the overflow.
  canvas.style.width = canvas.style.height = `${Math.floor(px / dpr)}px`;

  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, px, px);   // the quiet zone is part of the code
  ctx.fillStyle = '#000';
  for (let r = 0; r < n; r++) {
    for (let c = 0; c < n; c++) {
      if (qr.isDark(r, c)) {
        ctx.fillRect((c + QUIET_MODULES) * scale, (r + QUIET_MODULES) * scale, scale, scale);
      }
    }
  }

  canvas.classList.add('ready');
  els.qrPlaceholder.classList.add('hidden');
  setPill(els.qrState, 'encoded', 'ok');
  els.qrMeta.textContent =
    `version ${(n - 17) / 4} · ${n}×${n} modules · level ${ECC_LEVEL} · ${byteLength(text)} bytes`;
  els.download.disabled = false;
}

// ---------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------
// Only forward a deviceId we actually enumerated — before the first successful
// getUserMedia() the dropdown still holds its placeholder, and passing that as
// an exact deviceId constraint fails with OverconstrainedError.
function selectedDeviceId() {
  const id = els.cameraSelect.value;
  return cameras.some((c) => c.deviceId === id) ? id : null;
}

els.start.addEventListener('click', () => start(selectedDeviceId()));
els.stop.addEventListener('click', stop);

els.switch.addEventListener('click', () => {
  if (cameras.length < 2) return;
  const i = cameras.findIndex((c) => c.deviceId === currentDeviceId);
  const next = cameras[(i + 1) % cameras.length];
  els.cameraSelect.value = next.deviceId;
  start(next.deviceId);
});

els.cameraSelect.addEventListener('change', () => {
  const id = selectedDeviceId();
  if (stream && id) start(id);
});

els.copy.addEventListener('click', async () => {
  try {
    await navigator.clipboard.writeText(els.result.value);
    setPill(els.resultState, 'copied', 'ok');
  } catch {
    els.result.select();
    setPill(els.resultState, 'press ⌘/Ctrl+C', 'err');
  }
});

els.clear.addEventListener('click', () => {
  lastText = null;   // so the code currently in frame can be picked up again
  setContent('', 'typed');
  els.result.focus();
});

// Re-encode as the user types. Debounced: every keystroke would otherwise redo
// the Reed-Solomon encode and repaint the canvas.
let typingTimer = null;
els.result.addEventListener('input', () => {
  clearTimeout(typingTimer);
  typingTimer = setTimeout(() => {
    const text = els.result.value;
    updateContentMeta(text, 'typed');
    renderQrCode(text);
  }, 120);
});

// The code is sized from measured space, so re-fit it when that space changes —
// rotation, a resized window, or the on-screen keyboard opening.
let resizeTimer = null;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => renderQrCode(els.result.value), 150);
});

els.download.addEventListener('click', () => {
  const link = document.createElement('a');
  link.download = 'qr-code.png';
  link.href = els.qrCanvas.toDataURL('image/png');
  link.click();
});

// Releasing the camera when the page is hidden keeps the OS indicator honest
// and stops the loop from burning battery in the background.
document.addEventListener('visibilitychange', () => {
  if (document.hidden && stream) {
    stop();
    setStatus('Camera released because the page went to the background.');
  }
});
window.addEventListener('pagehide', stopStream);

(async function init() {
  if (typeof qrcode !== 'function') {
    setPill(els.qrState, 'encoder missing', 'err');
    els.qrMeta.textContent = 'vendor/qrcode-generator.min.js failed to load — scanning still works.';
  } else {
    // Browsers restore textarea contents across a reload, so encode whatever is
    // already there instead of showing an empty box next to a stale code.
    const restored = els.result.value;
    if (restored) setContent(restored, 'typed'); else renderQrCode('');
  }

  try {
    await pickEngine();
  } catch (err) {
    setStatus(err.message, 'err');
    els.start.disabled = true;
    return;
  }
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.register('sw.js').catch(() => {}); // offline is best effort
  }
})();
