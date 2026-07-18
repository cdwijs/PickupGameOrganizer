#!/usr/bin/env python3
"""Build the PickupGameOrganizer agenda-view mockup.

Reads the individual SVG icon assets, composes them into a full-HD (1080x1920)
portrait HTML layout, renders it to PNG with headless Chromium, and also exports
a transparent PNG of each icon. Single source of truth: the SVG files in
assets/svg/.
"""
import os
import subprocess
import pathlib

ROOT = pathlib.Path(__file__).parent
SVG_DIR = ROOT / "assets" / "svg"
PNG_DIR = ROOT / "assets" / "png"
CHROMIUM = "chromium"

def load_svg(name: str) -> str:
    return (SVG_DIR / f"{name}.svg").read_text()

ICONS = {n.stem: load_svg(n.stem) for n in SVG_DIR.glob("*.svg")}

# ---- Game data -------------------------------------------------------------
GAMES = [
    dict(group="Terrible Football Haarlem", avatar="icon-group-football",
         color="#1d4ed8", day="Fri", time="19:00", players=15, going=True),
    dict(group="Terrible Football Haarlem", avatar="icon-group-football",
         color="#1d4ed8", day="Mon", time="19:00", players=5, going=False),
    dict(group="Club Foos", avatar="icon-group-foos",
         color="#d97706", day="Wed", time="20:00", players=7, going=False),
]

ACTION_ICONS = [("icon-pin", "Location"), ("icon-euro", "Costs"),
                ("icon-chat", "Chat"), ("icon-website", "Website")]

def card_html(g: dict) -> str:
    chips = "".join(
        f'<div class="chip" title="{label}"><span class="chip-ico">{ICONS[ic]}</span></div>'
        for ic, label in ACTION_ICONS
    )
    if g["going"]:
        status = (f'<div class="status going"><span class="tick">{ICONS["icon-tick"]}</span>'
                  f'<span>Going</span></div>')
    else:
        status = '<div class="status notgoing"><span>Not going</span></div>'
    return f'''
    <div class="card{' is-going' if g['going'] else ''}">
      <div class="avatar" style="background:{g['color']}">{ICONS[g['avatar']]}</div>
      <div class="mid">
        <div class="name">{g['group']}</div>
        <div class="when">{g['day']} &middot; {g['time']}</div>
        <div class="chips">{chips}</div>
      </div>
      <div class="right">
        <div class="players">
          <span class="p-ico">{ICONS['icon-players']}</span>
          <span class="p-num">{g['players']}</span>
          <span class="p-lbl">players</span>
        </div>
        {status}
      </div>
    </div>'''

cards = "\n".join(card_html(g) for g in GAMES)

HTML = f'''<!doctype html><html><head><meta charset="utf-8"><style>
  * {{ margin:0; padding:0; box-sizing:border-box; }}
  html,body {{ width:1080px; height:1920px; overflow:hidden; }}
  body {{
    font-family:'DejaVu Sans','Liberation Sans',sans-serif;
    background:#eef1f5; color:#0f172a;
    display:flex; flex-direction:column;
  }}
  /* ---- status bar ---- */
  .statusbar {{ height:56px; display:flex; align-items:center; justify-content:space-between;
                padding:0 48px; font-size:26px; font-weight:700; color:#0f172a; }}
  .statusbar .dots {{ letter-spacing:3px; }}
  /* ---- header ---- */
  header {{ padding:24px 48px 20px; }}
  header h1 {{ font-size:64px; font-weight:700; letter-spacing:-1px; }}
  header .sub {{ font-size:30px; color:#64748b; margin-top:8px; font-weight:600; }}
  .weekpill {{ display:inline-flex; align-items:center; gap:12px; margin-top:26px;
    background:#fff; border:2px solid #e2e8f0; border-radius:999px;
    padding:14px 30px; font-size:28px; font-weight:700; color:#334155; }}
  .weekpill .dot {{ width:14px; height:14px; border-radius:50%; background:#16a34a; }}
  /* ---- list ---- */
  .list {{ flex:1; display:flex; flex-direction:column; gap:40px; padding:26px 48px 20px; }}
  .card {{
    flex:1; background:#ffffff; border-radius:36px; padding:44px 46px;
    display:flex; align-items:center; gap:38px;
    box-shadow:0 18px 44px rgba(15,23,42,.08); border:2px solid #eef1f5;
  }}
  .card.is-going {{ border-color:#bbf7d0; box-shadow:0 18px 44px rgba(22,163,74,.14); }}
  .avatar {{ width:158px; height:158px; border-radius:50%; flex-shrink:0;
    display:flex; align-items:center; justify-content:center;
    box-shadow:0 10px 22px rgba(15,23,42,.18); }}
  .avatar svg {{ width:92px; height:92px; color:#ffffff; }}
  .mid {{ flex:1; min-width:0; }}
  .name {{ font-size:44px; font-weight:700; line-height:1.12; letter-spacing:-.5px; }}
  .when {{ font-size:33px; font-weight:700; color:#2563eb; margin-top:10px; }}
  .card.is-going .when {{ color:#16a34a; }}
  .chips {{ display:flex; gap:20px; margin-top:30px; }}
  .chip {{ width:82px; height:82px; border-radius:22px; background:#f1f5f9;
    display:flex; align-items:center; justify-content:center; }}
  .chip-ico svg {{ width:44px; height:44px; color:#475569; display:block; }}
  .right {{ flex-shrink:0; display:flex; flex-direction:column; align-items:flex-end;
    justify-content:space-between; align-self:stretch; min-width:210px; }}
  .players {{ display:flex; flex-direction:column; align-items:flex-end; }}
  .players .p-ico svg {{ width:40px; height:40px; color:#94a3b8; }}
  .players .p-num {{ font-size:56px; font-weight:700; line-height:1; margin-top:6px; }}
  .players .p-lbl {{ font-size:27px; font-weight:600; color:#94a3b8; margin-top:2px; }}
  .status {{ display:flex; align-items:center; gap:14px; border-radius:999px;
    padding:18px 30px; font-size:31px; font-weight:700; }}
  .status.going {{ background:#16a34a; color:#fff; box-shadow:0 10px 22px rgba(22,163,74,.35); }}
  .status.going .tick svg {{ width:38px; height:38px; color:#fff; display:block; }}
  .status.notgoing {{ background:#eef2f6; color:#94a3b8; border:2px solid #e2e8f0; }}
  /* ---- bottom nav ---- */
  nav {{ height:150px; background:#ffffff; border-top:2px solid #e7ebf0;
    display:flex; align-items:center; justify-content:space-around; padding-bottom:16px; }}
  nav .item {{ display:flex; flex-direction:column; align-items:center; gap:8px;
    font-size:24px; font-weight:700; color:#94a3b8; }}
  nav .item.active {{ color:#16a34a; }}
  nav .item svg {{ width:44px; height:44px; }}
</style></head><body>
  <div class="statusbar"><span>9:41</span><span class="dots">&#9679;&#9679;&#9679; &#9723;</span></div>
  <header>
    <h1>Agenda</h1>
    <div class="sub">Your upcoming games</div>
    <div class="weekpill"><span class="dot"></span>This week</div>
  </header>
  <div class="list">
    {cards}
  </div>
  <nav>
    <div class="item active">{ICONS['icon-players']}<span>Agenda</span></div>
    <div class="item">{ICONS['icon-website']}<span>Discover</span></div>
    <div class="item">{ICONS['icon-pin']}<span>Nearby</span></div>
    <div class="item">{ICONS['icon-chat']}<span>Chats</span></div>
  </nav>
</body></html>'''

html_path = ROOT / "agenda-mockup.html"
html_path.write_text(HTML)
print("wrote", html_path)

# ---- render the full mockup ------------------------------------------------
out_png = ROOT / "agenda-mockup.png"
subprocess.run([
    CHROMIUM, "--headless=new", "--no-sandbox", "--hide-scrollbars",
    "--force-device-scale-factor=1", "--window-size=1080,1920",
    f"--screenshot={out_png}", f"file://{html_path}",
], check=True, capture_output=True)
print("rendered", out_png)

# ---- export a transparent PNG of every icon --------------------------------
for name, svg in ICONS.items():
    # avatar glyphs render white -> use a dark stroke for the standalone chip
    color = "#334155"
    wrap = f'''<!doctype html><html><head><meta charset="utf-8"><style>
      *{{margin:0;padding:0}} html,body{{width:200px;height:200px}}
      .b{{width:200px;height:200px;display:flex;align-items:center;justify-content:center;color:{color}}}
      .b svg{{width:150px;height:150px}}</style></head>
      <body><div class="b">{svg}</div></body></html>'''
    wp = PNG_DIR / f"{name}.html"
    wp.write_text(wrap)
    subprocess.run([
        CHROMIUM, "--headless=new", "--no-sandbox", "--hide-scrollbars",
        "--force-device-scale-factor=1", "--window-size=200,200",
        "--default-background-color=00000000",
        f"--screenshot={PNG_DIR / (name + '.png')}", f"file://{wp}",
    ], check=True, capture_output=True)
    wp.unlink()
print("exported", len(ICONS), "icon PNGs")
