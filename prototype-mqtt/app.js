// MQTT prototype: encrypted messaging via a public broker.
// Broker only ever sees ciphertext; AES-GCM key is shared out-of-band.

// ─── DOM handles ────────────────────────────────────────────────────────────
const $ = (id) => document.getElementById(id);
const cfg = {
  preset:     $("cfg-preset"),
  url:        $("cfg-url"),
  user:       $("cfg-user"),
  pass:       $("cfg-pass"),
  client:     $("cfg-client"),
  topicPub:   $("cfg-topic-pub"),
  topicSub:   $("cfg-topic-sub"),
  qos:        $("cfg-qos"),
  keepalive:  $("cfg-keepalive"),
  lwtTopic:   $("cfg-lwt-topic"),
  lwtPayload: $("cfg-lwt-payload"),
  retained:   $("cfg-retained"),
};
const ui = {
  connPill:    $("conn-pill"),
  btnConnect:  $("btn-connect"),
  btnDisc:     $("btn-disconnect"),
  keyPill:     $("key-pill"),
  keyB64:      $("key-b64"),
  btnGenKey:   $("btn-gen-key"),
  btnCopyKey:  $("btn-copy-key"),
  txPlain:     $("tx-plain"),
  txCipher:    $("tx-cipher"),
  btnSend:     $("btn-send"),
  btnClearTx:  $("btn-clear-tx"),
  rxPill:      $("rx-pill"),
  rxCipher:    $("rx-cipher"),
  rxPlain:     $("rx-plain"),
  log:         $("log"),
};

// ─── Broker presets ─────────────────────────────────────────────────────────
// The public brokers below are anonymous — user/pass are ignored. freemqtt
// requires the shared demo credentials.
const BROKER_PRESETS = {
  freemqtt:  { url: "wss://broker.freemqtt.com:8084/mqtt", user: "freemqtt", pass: "public" },
  emqx:      { url: "wss://broker.emqx.io:8084/mqtt",      user: "",         pass: ""       },
  hivemq:    { url: "wss://broker.hivemq.com:8884/mqtt",   user: "",         pass: ""       },
  mosquitto: { url: "wss://test.mosquitto.org:8081/",      user: "",         pass: ""       },
};

function applyPreset(name) {
  const p = BROKER_PRESETS[name];
  if (!p) return;
  cfg.url.value  = p.url;
  cfg.user.value = p.user;
  cfg.pass.value = p.pass;
}

// If the user hand-edits URL/user/pass, switch the dropdown to "custom" so
// the preset doesn't silently clobber the edits next render.
function markCustomIfDiverged() {
  const cur = { url: cfg.url.value, user: cfg.user.value, pass: cfg.pass.value };
  const p = BROKER_PRESETS[cfg.preset.value];
  if (!p) return;
  if (cur.url !== p.url || cur.user !== p.user || cur.pass !== p.pass) {
    cfg.preset.value = "custom";
  }
}

// ─── Logging ────────────────────────────────────────────────────────────────
function log(msg, cls = "") {
  const line = document.createElement("div");
  if (cls) line.className = cls;
  const ts = new Date().toISOString().slice(11, 19);
  line.textContent = `[${ts}] ${msg}`;
  ui.log.appendChild(line);
  ui.log.scrollTop = ui.log.scrollHeight;
}

// ─── Base64 helpers ────────────────────────────────────────────────────────
function bytesToB64(bytes) {
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin);
}
function b64ToBytes(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

// ─── Encryption ─────────────────────────────────────────────────────────────
// AES-GCM, 256-bit key, 12-byte IV. Wire format: [IV(12) | ciphertext | tag(16)]
// (WebCrypto appends the tag onto the ciphertext, so we just concat IV + result.)
let cryptoKey = null; // CryptoKey once imported

async function importKeyFromB64(b64) {
  const raw = b64ToBytes(b64);
  if (raw.length !== 32) throw new Error(`key must be 32 bytes (got ${raw.length})`);
  return await crypto.subtle.importKey(
    "raw", raw, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]
  );
}
async function encrypt(plaintext) {
  if (!cryptoKey) throw new Error("no encryption key set");
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const pt = new TextEncoder().encode(plaintext);
  const ct = new Uint8Array(await crypto.subtle.encrypt(
    { name: "AES-GCM", iv }, cryptoKey, pt
  ));
  const wire = new Uint8Array(iv.length + ct.length);
  wire.set(iv, 0);
  wire.set(ct, iv.length);
  return bytesToB64(wire);
}
async function decrypt(b64) {
  if (!cryptoKey) throw new Error("no decryption key set");
  const wire = b64ToBytes(b64);
  if (wire.length < 12 + 16) throw new Error("ciphertext too short");
  const iv = wire.slice(0, 12);
  const ct = wire.slice(12);
  const pt = await crypto.subtle.decrypt(
    { name: "AES-GCM", iv }, cryptoKey, ct
  );
  return new TextDecoder().decode(pt);
}

async function setKeyFromInput() {
  const b64 = ui.keyB64.value.trim();
  if (!b64) {
    cryptoKey = null;
    ui.keyPill.textContent = "no key";
    ui.keyPill.className = "pill warn";
    updateSendButton();
    return;
  }
  try {
    cryptoKey = await importKeyFromB64(b64);
    ui.keyPill.textContent = "key loaded";
    ui.keyPill.className = "pill ok";
    log("Encryption key loaded", "");
    // Refresh the ciphertext preview if there's plaintext waiting.
    refreshCipherPreview();
  } catch (e) {
    cryptoKey = null;
    ui.keyPill.textContent = "invalid key";
    ui.keyPill.className = "pill err";
    log(`Key error: ${e.message}`, "err");
  }
  updateSendButton();
}

function generateKey() {
  const raw = crypto.getRandomValues(new Uint8Array(32));
  ui.keyB64.value = bytesToB64(raw);
  setKeyFromInput();
}

// ─── Client ID ──────────────────────────────────────────────────────────────
function randomClientId() {
  const rnd = crypto.getRandomValues(new Uint8Array(6));
  return "proto-" + bytesToB64(rnd).replace(/[+/=]/g, "").slice(0, 8);
}

// ─── MQTT ───────────────────────────────────────────────────────────────────
let client = null;

function updateConnPill(state, cls = "") {
  ui.connPill.textContent = state;
  ui.connPill.className = "pill" + (cls ? " " + cls : "");
}

function updateSendButton() {
  const canSend = client && client.connected && cryptoKey && ui.txPlain.value.length > 0;
  ui.btnSend.disabled = !canSend;
}

function connect() {
  if (client) {
    log("Already connected or connecting", "warn");
    return;
  }

  const url = cfg.url.value.trim();
  const clientId = cfg.client.value.trim() || randomClientId();
  cfg.client.value = clientId;

  const qos = Math.max(0, Math.min(1, parseInt(cfg.qos.value, 10) || 0));
  const keepalive = Math.max(10, Math.min(300, parseInt(cfg.keepalive.value, 10) || 60));

  const opts = {
    clientId,
    username: cfg.user.value,
    password: cfg.pass.value,
    keepalive,
    clean: true,           // no retained session state
    protocolVersion: 4,    // MQTT 3.1.1
    reconnectPeriod: 1000, // MQTT.js base delay; we override below with jitter
    connectTimeout: 15000,
    will: cfg.lwtTopic.value.trim() ? {
      topic:   cfg.lwtTopic.value.trim(),
      payload: cfg.lwtPayload.value,
      qos,
      retain:  false,
    } : undefined,
  };

  log(`Connecting to ${url} as ${clientId}…`);
  updateConnPill("connecting", "warn");
  ui.btnConnect.disabled = true;
  // Disconnect doubles as "cancel" while we're still trying to connect.
  ui.btnDisc.disabled = false;
  ui.btnDisc.textContent = "Cancel connect";

  try {
    client = mqtt.connect(url, opts);
  } catch (e) {
    log(`Connect failed synchronously: ${e.message}`, "err");
    updateConnPill("disconnected", "err");
    ui.btnConnect.disabled = false;
    ui.btnDisc.disabled = true;
    ui.btnDisc.textContent = "Disconnect";
    client = null;
    return;
  }

  // Exponential backoff with jitter — override mqtt.js's fixed reconnect delay.
  let backoffMs = 1000;
  const backoffMax = 30000;

  client.on("connect", () => {
    log(`Connected (session present: ${client.connected})`, "recv");
    updateConnPill("connected", "ok");
    backoffMs = 1000;
    ui.btnDisc.disabled = false;
    ui.btnDisc.textContent = "Disconnect";

    const sub = cfg.topicSub.value.trim();
    if (sub) {
      client.subscribe(sub, { qos }, (err) => {
        if (err) log(`Subscribe error: ${err.message}`, "err");
        else    log(`Subscribed to ${sub} (QoS ${qos})`);
      });
    }
    updateSendButton();
  });

  client.on("reconnect", () => {
    const jitter = Math.floor(Math.random() * backoffMs * 0.3);
    const delay = backoffMs + jitter;
    log(`Reconnecting in ~${delay}ms (backoff ${backoffMs}ms + jitter)`, "warn");
    client.options.reconnectPeriod = delay;
    backoffMs = Math.min(backoffMs * 2, backoffMax);
    updateConnPill("reconnecting", "warn");
  });

  client.on("close",  () => { log("Connection closed", "warn"); updateConnPill("disconnected"); updateSendButton(); });
  client.on("offline",() => { log("Client offline", "warn"); updateConnPill("offline", "warn"); updateSendButton(); });
  client.on("error",  (err) => { log(`Error: ${err.message}`, "err"); });

  client.on("message", async (topic, payload) => {
    const b64 = payload.toString();
    ui.rxCipher.value = b64;
    log(`← ${topic}  (${payload.length} B)`, "recv");
    if (!cryptoKey) {
      ui.rxPlain.value = "(no key set — cannot decrypt)";
      ui.rxPill.textContent = "no key";
      ui.rxPill.className = "pill warn";
      return;
    }
    try {
      ui.rxPlain.value = await decrypt(b64);
      ui.rxPill.textContent = "decrypted";
      ui.rxPill.className = "pill ok";
    } catch (e) {
      ui.rxPlain.value = `(decrypt failed: ${e.message})`;
      ui.rxPill.textContent = "decrypt error";
      ui.rxPill.className = "pill err";
      log(`Decrypt failed: ${e.message}`, "err");
    }
  });
}

function disconnect() {
  if (!client) return;
  // Force=true so we abort any in-flight reconnect wait instead of letting the
  // library finish its current backoff before honouring the disconnect.
  const wasConnected = client.connected;
  log(wasConnected ? "Disconnecting…" : "Cancelling connect…");
  ui.btnDisc.disabled = true;
  client.end(true, {}, () => {
    log(wasConnected ? "Disconnected" : "Connect cancelled");
    client = null;
    updateConnPill("disconnected");
    ui.btnConnect.disabled = false;
    ui.btnDisc.disabled = true;
    ui.btnDisc.textContent = "Disconnect";
    updateSendButton();
  });
}

// ─── Send flow ──────────────────────────────────────────────────────────────
async function refreshCipherPreview() {
  const pt = ui.txPlain.value;
  if (!pt) { ui.txCipher.value = ""; updateSendButton(); return; }
  if (!cryptoKey) { ui.txCipher.value = "(set an encryption key to preview ciphertext)"; updateSendButton(); return; }
  try {
    ui.txCipher.value = await encrypt(pt);
  } catch (e) {
    ui.txCipher.value = `(encrypt error: ${e.message})`;
  }
  updateSendButton();
}

async function send() {
  if (!client || !client.connected) { log("Not connected", "err"); return; }
  if (!cryptoKey) { log("No encryption key", "err"); return; }
  const pt = ui.txPlain.value;
  if (!pt) return;

  // Re-encrypt with a fresh IV at send time so the wire payload matches what
  // actually goes out (the preview in tx-cipher used an earlier IV).
  let b64;
  try {
    b64 = await encrypt(pt);
  } catch (e) {
    log(`Encrypt failed: ${e.message}`, "err");
    return;
  }

  const bytes = new TextEncoder().encode(b64).length;
  if (bytes > 64 * 1024) {
    log(`Payload ${bytes} B exceeds 64 KB limit — not sending`, "err");
    return;
  }

  ui.txCipher.value = b64;
  const topic = cfg.topicPub.value.trim();
  const qos = Math.max(0, Math.min(1, parseInt(cfg.qos.value, 10) || 0));
  const retain = cfg.retained.checked;

  client.publish(topic, b64, { qos, retain }, (err) => {
    if (err) log(`Publish error: ${err.message}`, "err");
    else     log(`→ ${topic}  (${bytes} B, QoS ${qos}${retain ? ", retained" : ""})`, "send");
  });
}

// ─── Wire up UI ─────────────────────────────────────────────────────────────
ui.btnConnect.addEventListener("click", connect);
ui.btnDisc.addEventListener("click", disconnect);
ui.btnGenKey.addEventListener("click", generateKey);
ui.keyB64.addEventListener("input", setKeyFromInput);
ui.btnCopyKey.addEventListener("click", async () => {
  if (!ui.keyB64.value) return;
  try {
    await navigator.clipboard.writeText(ui.keyB64.value);
    log("Key copied to clipboard");
  } catch (e) {
    log(`Copy failed: ${e.message}`, "err");
  }
});
ui.txPlain.addEventListener("input", refreshCipherPreview);
ui.btnSend.addEventListener("click", send);
ui.btnClearTx.addEventListener("click", () => {
  ui.txPlain.value = "";
  ui.txCipher.value = "";
  updateSendButton();
});

cfg.preset.addEventListener("change", () => {
  if (cfg.preset.value !== "custom") applyPreset(cfg.preset.value);
});
for (const el of [cfg.url, cfg.user, cfg.pass]) {
  el.addEventListener("input", markCustomIfDiverged);
}

// ─── Init ───────────────────────────────────────────────────────────────────
cfg.client.value = randomClientId();
updateConnPill("disconnected");
setKeyFromInput();
