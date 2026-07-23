//! Arti P2P prototype.
//!
//! A single native (egui) program that demonstrates serverless peer-to-peer
//! connectivity over Tor onion services using Arti.
//!
//! - Press **Connect** with both fields empty  -> bootstrap Tor, launch an onion
//!   service, and report this node's onion address + libp2p-style multiaddr.
//! - Press **Connect** with the onion field (or the multiaddr) filled -> dial
//!   that peer over Tor.
//! - Press **Send increasing number every second** -> once connected, stream
//!   1, 2, 3, ... to the peer; the peer shows them in its status area.
//!
//! Data channel is a raw length-prefixed stream over the onion service (no
//! libp2p yet — the "libp2p address" field is just the dialable multiaddr).
//!
//! NOTE: Arti onion services are officially experimental. Fine for a prototype,
//! not for sensitive production traffic.

use std::sync::Arc;
use std::sync::mpsc as std_mpsc;
use std::time::Duration;

use eframe::egui;
use futures::StreamExt;
use tokio::io::{AsyncReadExt, AsyncWriteExt, ReadHalf, WriteHalf};
use tokio::sync::mpsc as tmpsc;
use tokio::sync::Mutex as AsyncMutex;

use arti_client::config::TorClientConfigBuilder;
use arti_client::{DataStream, TorClient, TorClientConfig};
use tor_cell::relaycell::msg::Connected;
use tor_hscrypto::pk::HsId;
use tor_hsservice::config::OnionServiceConfigBuilder;
use tor_hsservice::status::State as HsState;
use tor_hsservice::{HsNickname, RendRequest};
use tor_rtcompat::PreferredRuntime;

/// Virtual port the onion service exposes / peers dial.
const VIRT_PORT: u16 = 9999;
/// Onion service nickname (identifies the service in Arti's keystore).
const NICKNAME: &str = "pgo-arti-proto";
/// Guard against absurd frame sizes.
const MAX_FRAME: usize = 1024 * 1024;

/// Shared handle to the write half of the active connection.
type SharedWriter = Arc<AsyncMutex<Option<WriteHalf<DataStream>>>>;

// ---------------------------------------------------------------------------
// Messages between UI and backend
// ---------------------------------------------------------------------------

/// UI -> backend commands.
enum Command {
    Connect { onion: String, libp2p: String },
    StartCounter,
}

/// Backend -> UI events.
enum UiEvent {
    Log(String),
    Recv(String),
    Onion(String),
    Multiaddr(String),
}

/// Sends events to the UI and wakes it up (egui only repaints on demand).
#[derive(Clone)]
struct Emitter {
    tx: std_mpsc::Sender<UiEvent>,
    ctx: egui::Context,
}

impl Emitter {
    fn send(&self, ev: UiEvent) {
        let _ = self.tx.send(ev);
        self.ctx.request_repaint();
    }
    fn log(&self, s: impl Into<String>) {
        self.send(UiEvent::Log(s.into()));
    }
    fn recv(&self, s: String) {
        self.send(UiEvent::Recv(s));
    }
}

// ---------------------------------------------------------------------------
// Tor helpers
// ---------------------------------------------------------------------------

/// Build a Tor client config with per-profile state/cache dirs so that two
/// instances can run on the same host (set ARTI_PROFILE to differ).
fn build_config() -> Result<TorClientConfig, String> {
    let profile = std::env::var("ARTI_PROFILE").unwrap_or_else(|_| "default".to_string());
    let base = dirs::data_dir()
        .unwrap_or_else(std::env::temp_dir)
        .join("arti-p2p-proto")
        .join(&profile);
    let state = base.join("state");
    let cache = base.join("cache");

    std::fs::create_dir_all(&state).map_err(|e| format!("create state dir: {e}"))?;
    std::fs::create_dir_all(&cache).map_err(|e| format!("create cache dir: {e}"))?;

    // Arti requires restrictive permissions on its state dir.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let perms = std::fs::Permissions::from_mode(0o700);
        std::fs::set_permissions(&state, perms.clone()).map_err(|e| format!("chmod state: {e}"))?;
        std::fs::set_permissions(&cache, perms).map_err(|e| format!("chmod cache: {e}"))?;
    }

    TorClientConfigBuilder::from_directories(state, cache)
        .build()
        .map_err(|e| format!("build config: {e}"))
}

async fn bootstrap(emit: &Emitter) -> Result<TorClient<PreferredRuntime>, String> {
    emit.log("Bootstrapping Tor (30-60s on first run)...");
    let config = build_config()?;
    let client = TorClient::create_bootstrapped(config)
        .await
        .map_err(|e| format!("bootstrap: {e}"))?;
    emit.log("Tor bootstrapped.");
    Ok(client)
}

/// Convert an `HsId` (32-byte ed25519 key) to its `<b32>.onion` address string.
fn hsid_to_onion(hsid: &HsId) -> String {
    use sha3::{Digest, Sha3_256};

    let pubkey: &[u8; 32] = hsid.as_ref();
    let version: u8 = 0x03;

    let mut hasher = Sha3_256::new();
    hasher.update(b".onion checksum");
    hasher.update(pubkey);
    hasher.update([version]);
    let checksum = hasher.finalize();

    let mut combined = [0u8; 35];
    combined[..32].copy_from_slice(pubkey);
    combined[32..34].copy_from_slice(&checksum[..2]);
    combined[34] = version;

    format!("{}.onion", base32_encode(&combined).to_lowercase())
}

/// RFC 4648 base32 (no padding).
fn base32_encode(data: &[u8]) -> String {
    const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    let mut out = String::new();
    let mut buffer: u64 = 0;
    let mut bits = 0;
    for &byte in data {
        buffer = (buffer << 8) | byte as u64;
        bits += 8;
        while bits >= 5 {
            bits -= 5;
            out.push(ALPHABET[((buffer >> bits) & 0x1f) as usize] as char);
        }
    }
    if bits > 0 {
        out.push(ALPHABET[((buffer << (5 - bits)) & 0x1f) as usize] as char);
    }
    out
}

/// `<b32>.onion` -> `/onion3/<b32>:<port>`.
fn onion_to_multiaddr(onion: &str, port: u16) -> String {
    let b32 = onion.strip_suffix(".onion").unwrap_or(onion);
    format!("/onion3/{b32}:{port}")
}

/// Work out the "host:port" dial target from whatever the user filled in.
fn normalize_target(onion: &str, libp2p: &str) -> String {
    let onion = onion.trim();
    if !onion.is_empty() {
        return if onion.contains(':') {
            onion.to_string()
        } else {
            format!("{onion}:{VIRT_PORT}")
        };
    }
    let s = libp2p.trim();
    if let Some(rest) = s.strip_prefix("/onion3/") {
        let rest = rest.trim_end_matches('/');
        let (addr, port) = rest.split_once(':').unwrap_or((rest, "9999"));
        return format!("{addr}.onion:{port}");
    }
    s.to_string()
}

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------

async fn backend_main(mut cmd_rx: tmpsc::UnboundedReceiver<Command>, emit: Emitter) {
    let writer: SharedWriter = Arc::new(AsyncMutex::new(None));
    let mut counter_started = false;

    while let Some(cmd) = cmd_rx.recv().await {
        match cmd {
            Command::Connect { onion, libp2p } => {
                let emit = emit.clone();
                let writer = writer.clone();
                // Spawn so the (slow) bootstrap doesn't block further commands.
                tokio::spawn(async move {
                    if onion.trim().is_empty() && libp2p.trim().is_empty() {
                        listen_mode(emit, writer).await;
                    } else {
                        dial_mode(normalize_target(&onion, &libp2p), emit, writer).await;
                    }
                });
            }
            Command::StartCounter => {
                if counter_started {
                    emit.log("Counter already running.");
                    continue;
                }
                counter_started = true;
                let emit = emit.clone();
                let writer = writer.clone();
                tokio::spawn(async move { counter_loop(emit, writer).await });
            }
        }
    }
}

/// Host an onion service and accept incoming peers.
async fn listen_mode(emit: Emitter, writer: SharedWriter) {
    let client = match bootstrap(&emit).await {
        Ok(c) => c,
        Err(e) => return emit.log(format!("ERROR: {e}")),
    };

    let nickname = match HsNickname::new(NICKNAME.to_string()) {
        Ok(n) => n,
        Err(e) => return emit.log(format!("ERROR: bad nickname: {e}")),
    };
    let hs_config = match OnionServiceConfigBuilder::default().nickname(nickname).build() {
        Ok(c) => c,
        Err(e) => return emit.log(format!("ERROR: onion config: {e}")),
    };

    let (service, rend_requests) = match client.launch_onion_service(hs_config) {
        Ok(Some(pair)) => pair,
        Ok(None) => return emit.log("ERROR: onion service disabled in config"),
        Err(e) => return emit.log(format!("ERROR: launch onion service: {e}")),
    };

    let onion = service
        .onion_address()
        .map(|id| hsid_to_onion(&id))
        .unwrap_or_else(|| "generating...".to_string());
    let multiaddr = onion_to_multiaddr(&onion, VIRT_PORT);

    emit.send(UiEvent::Onion(onion.clone()));
    emit.send(UiEvent::Multiaddr(multiaddr.clone()));
    emit.log(format!("Onion service launching at {onion}:{VIRT_PORT}"));
    emit.log(format!("libp2p multiaddr: {multiaddr}"));
    emit.log("Publishing descriptor to the Tor network (can take 1-2 min)...");

    // Poll until the descriptor is published / reachable.
    {
        let service = service.clone();
        let emit = emit.clone();
        tokio::spawn(async move {
            loop {
                match service.status().state() {
                    HsState::Running | HsState::DegradedReachable => {
                        emit.log("Onion service PUBLISHED — peers can connect now.");
                        break;
                    }
                    HsState::Broken => {
                        emit.log("ERROR: onion service entered Broken state.");
                        break;
                    }
                    _ => tokio::time::sleep(Duration::from_secs(2)).await,
                }
            }
        });
    }

    emit.log("Listening for incoming connections...");
    let mut rends = Box::pin(rend_requests);
    while let Some(rend) = rends.next().await {
        match accept_stream(rend).await {
            Ok(stream) => {
                emit.log("Incoming peer connected.");
                install_stream(stream, emit.clone(), writer.clone()).await;
            }
            Err(e) => emit.log(format!("accept failed: {e}")),
        }
    }
    emit.log("Onion request stream ended.");
}

/// Turn one rendezvous request into a ready DataStream.
async fn accept_stream(rend: RendRequest) -> Result<DataStream, String> {
    let mut streams = rend.accept().await.map_err(|e| format!("accept rend: {e}"))?;
    let req = streams
        .next()
        .await
        .ok_or_else(|| "no stream request".to_string())?;
    req.accept(Connected::new_empty())
        .await
        .map_err(|e| format!("accept stream: {e}"))
}

/// Dial a remote onion peer.
async fn dial_mode(target: String, emit: Emitter, writer: SharedWriter) {
    let client = match bootstrap(&emit).await {
        Ok(c) => c,
        Err(e) => return emit.log(format!("ERROR: {e}")),
    };
    emit.log(format!("Connecting to {target} (routing through Tor)..."));
    match client.connect(target.as_str()).await {
        Ok(stream) => {
            emit.log(format!("Connected to {target}."));
            install_stream(stream, emit, writer).await;
        }
        Err(e) => emit.log(format!("ERROR: connect to {target}: {e}")),
    }
}

/// Register a fresh connection: keep the write half, read from the read half.
async fn install_stream(stream: DataStream, emit: Emitter, writer: SharedWriter) {
    let (read_half, write_half) = tokio::io::split(stream);
    *writer.lock().await = Some(write_half);
    emit.log("Connection ready — you can now send the counter.");
    tokio::spawn(async move { reader_loop(read_half, emit).await });
}

/// Read length-prefixed frames and surface them to the UI.
async fn reader_loop(mut read_half: ReadHalf<DataStream>, emit: Emitter) {
    loop {
        let mut len_buf = [0u8; 4];
        if let Err(e) = read_half.read_exact(&mut len_buf).await {
            emit.log(format!("Connection closed: {e}"));
            return;
        }
        let len = u32::from_le_bytes(len_buf) as usize;
        if len > MAX_FRAME {
            emit.log(format!("Frame too large ({len} bytes), closing."));
            return;
        }
        let mut buf = vec![0u8; len];
        if let Err(e) = read_half.read_exact(&mut buf).await {
            emit.log(format!("Read error: {e}"));
            return;
        }
        emit.recv(String::from_utf8_lossy(&buf).into_owned());
    }
}

/// Send an increasing number once per second on the active connection.
async fn counter_loop(emit: Emitter, writer: SharedWriter) {
    emit.log("Counter started — sending an increasing number every second.");
    let mut n: u64 = 0;
    let mut warned_idle = false;
    let mut ticker = tokio::time::interval(Duration::from_secs(1));
    loop {
        ticker.tick().await;
        let mut guard = writer.lock().await;
        let Some(w) = guard.as_mut() else {
            if !warned_idle {
                emit.log("Counter is waiting for a connection...");
                warned_idle = true;
            }
            continue;
        };
        warned_idle = false;
        n += 1;
        let payload = n.to_string();
        let len = (payload.len() as u32).to_le_bytes();
        if let Err(e) = w.write_all(&len).await {
            return emit.log(format!("Send failed: {e}"));
        }
        if let Err(e) = w.write_all(payload.as_bytes()).await {
            return emit.log(format!("Send failed: {e}"));
        }
        let _ = w.flush().await;
        emit.log(format!("Sent: {n}"));
    }
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------

struct App {
    onion_input: String,
    libp2p_input: String,
    status_log: String,
    cmd_tx: tmpsc::UnboundedSender<Command>,
    evt_rx: std_mpsc::Receiver<UiEvent>,
}

impl App {
    fn append(&mut self, line: String) {
        self.status_log.push_str(&line);
        self.status_log.push('\n');
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Drain backend events.
        while let Ok(ev) = self.evt_rx.try_recv() {
            match ev {
                UiEvent::Log(s) => self.append(format!("• {s}")),
                UiEvent::Recv(s) => self.append(format!("⟵ received: {s}")),
                UiEvent::Onion(s) => self.onion_input = s,
                UiEvent::Multiaddr(s) => self.libp2p_input = s,
            }
        }

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.heading("Arti P2P Prototype");
            ui.label(
                "Leave both fields empty and press Connect to start Tor and host an onion \
                 service (fields fill in with this node's address).\n\
                 Paste a peer's onion address (or /onion3 multiaddr) and press Connect to dial it.",
            );
            ui.add_space(8.0);

            egui::Grid::new("fields")
                .num_columns(2)
                .spacing([8.0, 6.0])
                .show(ui, |ui| {
                    ui.label("Onion address:");
                    ui.add(
                        egui::TextEdit::singleline(&mut self.onion_input)
                            .hint_text("abc…xyz.onion   (empty = host a new one)")
                            .desired_width(f32::INFINITY),
                    );
                    ui.end_row();

                    ui.label("libp2p multiaddr:");
                    ui.add(
                        egui::TextEdit::singleline(&mut self.libp2p_input)
                            .hint_text("/onion3/abc…xyz:9999")
                            .desired_width(f32::INFINITY),
                    );
                    ui.end_row();
                });

            ui.add_space(8.0);
            ui.horizontal(|ui| {
                if ui.button("Connect").clicked() {
                    self.append("• Connect pressed".to_string());
                    let _ = self.cmd_tx.send(Command::Connect {
                        onion: self.onion_input.clone(),
                        libp2p: self.libp2p_input.clone(),
                    });
                }
                if ui.button("Send increasing number every second").clicked() {
                    let _ = self.cmd_tx.send(Command::StartCounter);
                }
            });

            ui.add_space(8.0);
            ui.label("Status / received data:");
            egui::ScrollArea::vertical()
                .auto_shrink([false, false])
                .stick_to_bottom(true)
                .show(ui, |ui| {
                    ui.add(
                        egui::TextEdit::multiline(&mut self.status_log)
                            .desired_rows(16)
                            .desired_width(f32::INFINITY)
                            .font(egui::TextStyle::Monospace)
                            .interactive(false),
                    );
                });
        });
    }
}

fn main() -> eframe::Result<()> {
    let (cmd_tx, cmd_rx) = tmpsc::unbounded_channel::<Command>();
    let (evt_tx, evt_rx) = std_mpsc::channel::<UiEvent>();

    // These get moved into the eframe creator closure exactly once.
    let mut cmd_rx = Some(cmd_rx);
    let mut evt_tx = Some(evt_tx);
    let mut app_parts = Some((cmd_tx, evt_rx));

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([680.0, 560.0]),
        ..Default::default()
    };

    eframe::run_native(
        "Arti P2P Prototype",
        options,
        Box::new(move |cc| {
            // Start the tokio backend on its own thread.
            let ctx = cc.egui_ctx.clone();
            let cmd_rx = cmd_rx.take().unwrap();
            let evt_tx = evt_tx.take().unwrap();
            std::thread::spawn(move || {
                let rt = tokio::runtime::Runtime::new().expect("build tokio runtime");
                rt.block_on(backend_main(cmd_rx, Emitter { tx: evt_tx, ctx }));
            });

            let (cmd_tx, evt_rx) = app_parts.take().unwrap();
            Ok(Box::new(App {
                onion_input: String::new(),
                libp2p_input: String::new(),
                status_log: String::new(),
                cmd_tx,
                evt_rx,
            }))
        }),
    )
}
