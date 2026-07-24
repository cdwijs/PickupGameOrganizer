//! Arti P2P core exposed to Android over JNI.
//!
//! This reuses the exact onion-service networking of the desktop prototype
//! (`prototype-arti`) but replaces the egui event channel with a global event
//! queue that Kotlin drains by polling `nativePoll()`.
//!
//! Events are lines of the form `TAG\u{1}BODY`, where TAG is one of:
//!   LOG    — a status message
//!   RECV   — data received from the peer
//!   ONION  — this node's .onion address (host mode)
//!   MADDR  — this node's /onion3 multiaddr (host mode)
//!
//! JNI entry points (class `org.pgo.artip2p.Native`):
//!   nativeInit(dir)                 — set Arti state/cache base dir (app filesDir)
//!   nativeConnect(onion, libp2p)    — empty => host; filled => dial
//!   nativeStartCounter()            — stream an increasing number every second
//!   nativePoll() -> String          — drain queued events ('\n'-separated)

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::sync::Mutex as StdMutex;
use std::time::Duration;

use futures::StreamExt;
use once_cell::sync::Lazy;
use tokio::io::{AsyncReadExt, AsyncWriteExt, ReadHalf, WriteHalf};
use tokio::runtime::Runtime;
use tokio::sync::Mutex as AsyncMutex;

use arti_client::config::TorClientConfigBuilder;
use arti_client::{DataStream, TorClient, TorClientConfig};
use tor_cell::relaycell::msg::Connected;
use tor_hscrypto::pk::HsId;
use tor_hsservice::config::OnionServiceConfigBuilder;
use tor_hsservice::status::State as HsState;
use tor_hsservice::{HsNickname, RendRequest};
use tor_rtcompat::PreferredRuntime;

use jni::objects::{JClass, JString};
use jni::sys::jstring;
use jni::JNIEnv;

const VIRT_PORT: u16 = 9999;
const NICKNAME: &str = "pgo-arti-proto";
const MAX_FRAME: usize = 1024 * 1024;
const SEP: char = '\u{1}';

type SharedWriter = Arc<AsyncMutex<Option<WriteHalf<DataStream>>>>;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static RT: Lazy<Runtime> = Lazy::new(|| {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("build tokio runtime")
});
static EVENTS: Lazy<StdMutex<VecDeque<String>>> = Lazy::new(|| StdMutex::new(VecDeque::new()));
static WRITER: Lazy<SharedWriter> = Lazy::new(|| Arc::new(AsyncMutex::new(None)));
static BASE_DIR: Lazy<StdMutex<Option<String>>> = Lazy::new(|| StdMutex::new(None));
static COUNTER_STARTED: AtomicBool = AtomicBool::new(false);

fn emit(tag: &str, body: impl AsRef<str>) {
    let line = format!("{tag}{SEP}{}", body.as_ref());
    if let Ok(mut q) = EVENTS.lock() {
        q.push_back(line);
    }
}
fn log(msg: impl AsRef<str>) {
    emit("LOG", msg);
}

// ---------------------------------------------------------------------------
// Tor helpers (mirrors the desktop prototype)
// ---------------------------------------------------------------------------

fn build_config() -> Result<TorClientConfig, String> {
    let base = BASE_DIR
        .lock()
        .ok()
        .and_then(|g| g.clone())
        .ok_or_else(|| "nativeInit was not called (no state dir)".to_string())?;
    let base = std::path::Path::new(&base).join("arti");
    let state = base.join("state");
    let cache = base.join("cache");

    std::fs::create_dir_all(&state).map_err(|e| format!("create state dir: {e}"))?;
    std::fs::create_dir_all(&cache).map_err(|e| format!("create cache dir: {e}"))?;

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let perms = std::fs::Permissions::from_mode(0o700);
        let _ = std::fs::set_permissions(&state, perms.clone());
        let _ = std::fs::set_permissions(&cache, perms);
    }

    TorClientConfigBuilder::from_directories(state, cache)
        .build()
        .map_err(|e| format!("build config: {e}"))
}

async fn bootstrap() -> Result<TorClient<PreferredRuntime>, String> {
    log("Bootstrapping Tor (can take 30-90s on mobile)...");
    let config = build_config()?;
    let client = TorClient::create_bootstrapped(config)
        .await
        .map_err(|e| format!("bootstrap: {e}"))?;
    log("Tor bootstrapped.");
    Ok(client)
}

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

fn onion_to_multiaddr(onion: &str, port: u16) -> String {
    let b32 = onion.strip_suffix(".onion").unwrap_or(onion);
    format!("/onion3/{b32}:{port}")
}

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
// Networking tasks
// ---------------------------------------------------------------------------

async fn listen_mode(writer: SharedWriter) {
    let client = match bootstrap().await {
        Ok(c) => c,
        Err(e) => return log(format!("ERROR: {e}")),
    };

    let nickname = match HsNickname::new(NICKNAME.to_string()) {
        Ok(n) => n,
        Err(e) => return log(format!("ERROR: bad nickname: {e}")),
    };
    let hs_config = match OnionServiceConfigBuilder::default().nickname(nickname).build() {
        Ok(c) => c,
        Err(e) => return log(format!("ERROR: onion config: {e}")),
    };

    let (service, rend_requests) = match client.launch_onion_service(hs_config) {
        Ok(Some(pair)) => pair,
        Ok(None) => return log("ERROR: onion service disabled in config"),
        Err(e) => return log(format!("ERROR: launch onion service: {e}")),
    };

    let onion = service
        .onion_address()
        .map(|id| hsid_to_onion(&id))
        .unwrap_or_else(|| "generating...".to_string());
    let multiaddr = onion_to_multiaddr(&onion, VIRT_PORT);

    emit("ONION", &onion);
    emit("MADDR", &multiaddr);
    log(format!("Onion service launching at {onion}:{VIRT_PORT}"));
    log("Publishing descriptor to the Tor network (can take 1-2 min)...");

    {
        let service = service.clone();
        RT.spawn(async move {
            loop {
                match service.status().state() {
                    HsState::Running | HsState::DegradedReachable => {
                        log("Onion service PUBLISHED — peers can connect now.");
                        break;
                    }
                    HsState::Broken => {
                        log("ERROR: onion service entered Broken state.");
                        break;
                    }
                    _ => tokio::time::sleep(Duration::from_secs(2)).await,
                }
            }
        });
    }

    log("Listening for incoming connections...");
    let mut rends = Box::pin(rend_requests);
    while let Some(rend) = rends.next().await {
        match accept_stream(rend).await {
            Ok(stream) => {
                log("Incoming peer connected.");
                install_stream(stream, writer.clone()).await;
            }
            Err(e) => log(format!("accept failed: {e}")),
        }
    }
    log("Onion request stream ended.");
}

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

async fn dial_mode(target: String, writer: SharedWriter) {
    let client = match bootstrap().await {
        Ok(c) => c,
        Err(e) => return log(format!("ERROR: {e}")),
    };
    log(format!("Connecting to {target} (routing through Tor)..."));
    match client.connect(target.as_str()).await {
        Ok(stream) => {
            log(format!("Connected to {target}."));
            install_stream(stream, writer).await;
        }
        Err(e) => log(format!("ERROR: connect to {target}: {e}")),
    }
}

async fn install_stream(stream: DataStream, writer: SharedWriter) {
    let (read_half, write_half) = tokio::io::split(stream);
    *writer.lock().await = Some(write_half);
    log("Connection ready — you can now send the counter.");
    RT.spawn(async move { reader_loop(read_half).await });
}

async fn reader_loop(mut read_half: ReadHalf<DataStream>) {
    loop {
        let mut len_buf = [0u8; 4];
        if let Err(e) = read_half.read_exact(&mut len_buf).await {
            log(format!("Connection closed: {e}"));
            return;
        }
        let len = u32::from_le_bytes(len_buf) as usize;
        if len > MAX_FRAME {
            log(format!("Frame too large ({len} bytes), closing."));
            return;
        }
        let mut buf = vec![0u8; len];
        if let Err(e) = read_half.read_exact(&mut buf).await {
            log(format!("Read error: {e}"));
            return;
        }
        emit("RECV", String::from_utf8_lossy(&buf).into_owned());
    }
}

async fn counter_loop(writer: SharedWriter) {
    log("Counter started — sending an increasing number every second.");
    let mut n: u64 = 0;
    let mut warned_idle = false;
    let mut ticker = tokio::time::interval(Duration::from_secs(1));
    loop {
        ticker.tick().await;
        let mut guard = writer.lock().await;
        let Some(w) = guard.as_mut() else {
            if !warned_idle {
                log("Counter is waiting for a connection...");
                warned_idle = true;
            }
            continue;
        };
        warned_idle = false;
        n += 1;
        let payload = n.to_string();
        let len = (payload.len() as u32).to_le_bytes();
        if w.write_all(&len).await.is_err() || w.write_all(payload.as_bytes()).await.is_err() {
            log("Send failed — connection lost.");
            return;
        }
        let _ = w.flush().await;
        log(format!("Sent: {n}"));
    }
}

// ---------------------------------------------------------------------------
// JNI entry points
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "system" fn Java_org_pgo_artip2p_Native_nativeInit<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
    dir: JString<'local>,
) {
    let dir: String = match env.get_string(&dir) {
        Ok(s) => s.into(),
        Err(_) => return,
    };
    if let Ok(mut g) = BASE_DIR.lock() {
        *g = Some(dir);
    }
    log("Native core initialised.");
}

#[no_mangle]
pub extern "system" fn Java_org_pgo_artip2p_Native_nativeConnect<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
    onion: JString<'local>,
    libp2p: JString<'local>,
) {
    let onion: String = env.get_string(&onion).map(|s| s.into()).unwrap_or_default();
    let libp2p: String = env.get_string(&libp2p).map(|s| s.into()).unwrap_or_default();

    let writer = WRITER.clone();
    RT.spawn(async move {
        if onion.trim().is_empty() && libp2p.trim().is_empty() {
            listen_mode(writer).await;
        } else {
            dial_mode(normalize_target(&onion, &libp2p), writer).await;
        }
    });
}

#[no_mangle]
pub extern "system" fn Java_org_pgo_artip2p_Native_nativeStartCounter<'local>(
    _env: JNIEnv<'local>,
    _class: JClass<'local>,
) {
    if COUNTER_STARTED.swap(true, Ordering::SeqCst) {
        log("Counter already running.");
        return;
    }
    let writer = WRITER.clone();
    RT.spawn(async move { counter_loop(writer).await });
}

#[no_mangle]
pub extern "system" fn Java_org_pgo_artip2p_Native_nativePoll<'local>(
    mut env: JNIEnv<'local>,
    _class: JClass<'local>,
) -> jstring {
    let out = match EVENTS.lock() {
        Ok(mut q) => q.drain(..).collect::<Vec<_>>().join("\n"),
        Err(_) => String::new(),
    };
    env.new_string(out)
        .map(|s| s.into_raw())
        .unwrap_or(std::ptr::null_mut())
}
