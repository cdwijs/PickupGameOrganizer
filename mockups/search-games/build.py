#!/usr/bin/env python3
"""Build the PickupGameOrganizer "Search games" mockup.

Filters at the top (search field + filter chips), then search-result cards. Each
result carries the relevant actions: a Go / Not go toggle (groups you belong to),
a Request-to-join button (groups you don't), and a Subscribe (bell) button.
Reuses the shared icon set from the other mockups; renders to a full-HD
(1080x1920) portrait PNG with headless Chromium and exports a transparent PNG of
each icon. Source of truth: assets/svg/.
"""
import subprocess
import pathlib

ROOT = pathlib.Path(__file__).parent
SVG_DIR = ROOT / "assets" / "svg"
PNG_DIR = ROOT / "assets" / "png"
CHROMIUM = "chromium"

I = {p.stem: p.read_text() for p in SVG_DIR.glob("*.svg")}

CHIPS = ["Sport: Soccer", "≤ 10 km", "This week"]

GAMES = [
    dict(group="Terrible Football Haarlem", icon="icon-group-football", color="#1d4ed8",
         sport="Soccer", when="Fri · 19:00", loc="Haarlem · 2.3 km",
         players="15/30", member=True, going=True, subscribed=True),
    dict(group="Club Foos", icon="icon-group-foos", color="#d97706",
         sport="Foosball", when="Wed · 20:00", loc="Haarlem · 3.1 km",
         players="7/12", member=False, subscribed=False),
    dict(group="Haarlem United", icon="icon-group-football", color="#16a34a",
         sport="Soccer", when="Sun · 10:30", loc="Haarlem · 5.0 km",
         players="18/22", member=False, subscribed=True),
]


def actions(g):
    if g["member"]:
        main = f'''<div class="seg">
          <div class="opt {'on' if g.get('going') else ''}">{I['icon-tick']}<span>Go</span></div>
          <div class="opt {'' if g.get('going') else 'on-no'}"><span>Not go</span></div>
        </div>'''
    else:
        main = f'<div class="join">{I["icon-join"]}<span>Request to join</span></div>'

    if g["subscribed"]:
        sub = f'<div class="heartbtn on">{I["icon-heart-fill"]}</div>'
    else:
        sub = f'<div class="heartbtn">{I["icon-heart"]}</div>'
    return main, sub


def card(g):
    main, sub = actions(g)
    return f'''
      <div class="card">
        <div class="top">
          <div class="ava" style="background:{g['color']}">{I[g['icon']]}</div>
          <div class="info">
            <div class="name">{g['group']}</div>
            <div class="when">{g['sport']} &middot; {g['when']}</div>
          </div>
          {sub}
        </div>
        <div class="meta">
          <span class="m"><span class="mi">{I['icon-pin']}</span>{g['loc']}</span>
          <span class="m"><span class="mi">{I['icon-players']}</span>{g['players']} players</span>
        </div>
        <div class="acts">
          {main}
        </div>
      </div>'''


cards = "\n".join(card(g) for g in GAMES)
chips = "".join(
    f'<div class="chip"><span>{c}</span>{I["icon-chevron-down"]}</div>' for c in CHIPS)

HTML = f'''<!doctype html><html><head><meta charset="utf-8"><style>
  * {{ margin:0; padding:0; box-sizing:border-box; }}
  html,body {{ width:1080px; height:1920px; overflow:hidden; }}
  body {{ font-family:'DejaVu Sans','Liberation Sans',sans-serif;
    background:#eef1f5; color:#0f172a; display:flex; flex-direction:column; }}

  .statusbar {{ height:56px; display:flex; align-items:center; justify-content:space-between;
    padding:0 48px; font-size:26px; font-weight:700; }}
  .statusbar .dots {{ letter-spacing:3px; }}
  header {{ padding:12px 48px 4px; }}
  header h1 {{ font-size:56px; font-weight:700; letter-spacing:-1px; }}

  /* filters */
  .filters {{ padding:14px 48px 6px; }}
  .searchbar {{ display:flex; align-items:center; gap:20px; background:#fff; border:2px solid #e2e8f0;
    border-radius:24px; height:96px; padding:0 26px; }}
  .searchbar .si svg {{ width:44px; height:44px; color:#64748b; display:block; }}
  .searchbar .q {{ flex:1; font-size:34px; font-weight:600; color:#0f172a; }}
  .searchbar .q.ph {{ color:#a3adba; font-weight:500; }}
  .chips {{ display:flex; align-items:center; gap:16px; margin-top:18px; }}
  .filterbtn {{ width:70px; height:70px; border-radius:18px; background:#1d4ed8; flex-shrink:0;
    display:flex; align-items:center; justify-content:center; box-shadow:0 8px 18px rgba(29,78,216,.3); }}
  .filterbtn svg {{ width:38px; height:38px; color:#fff; }}
  .heartchip {{ gap:10px; }}
  .heartchip .hh {{ display:inline-flex; }}
  .heartchip .hh svg {{ width:34px; height:34px; color:#ef4444; }}
  .chip {{ display:flex; align-items:center; gap:12px; background:#fff; border:2px solid #e2e8f0;
    border-radius:999px; padding:14px 24px; font-size:27px; font-weight:700; color:#334155; }}
  .chip svg {{ width:32px; height:32px; color:#94a3b8; }}

  .rescount {{ padding:20px 54px 6px; font-size:27px; font-weight:700; color:#64748b;
    text-transform:uppercase; letter-spacing:1.5px; }}

  /* results */
  .list {{ flex:1; padding:6px 48px 8px; display:flex; flex-direction:column; gap:30px;
    overflow:hidden; }}
  .card {{ background:#fff; border:2px solid #e6ebf1; border-radius:32px; padding:34px 34px;
    box-shadow:0 12px 30px rgba(15,23,42,.06); }}
  .top {{ display:flex; align-items:center; gap:26px; }}
  .ava {{ width:112px; height:112px; border-radius:50%; flex-shrink:0;
    display:flex; align-items:center; justify-content:center;
    box-shadow:0 8px 20px rgba(15,23,42,.18); }}
  .ava svg {{ width:64px; height:64px; color:#fff; }}
  .info {{ flex:1; min-width:0; }}
  .name {{ font-size:42px; font-weight:700; line-height:1.1; letter-spacing:-.5px; }}
  .when {{ font-size:30px; font-weight:700; color:#2563eb; margin-top:8px; }}

  .meta {{ display:flex; gap:40px; margin-top:24px; padding:0 4px; }}
  .meta .m {{ display:flex; align-items:center; gap:12px; font-size:29px; font-weight:600; color:#64748b; }}
  .meta .mi svg {{ width:38px; height:38px; color:#94a3b8; display:block; }}

  .acts {{ display:flex; gap:18px; margin-top:30px; }}
  /* go / not go segmented */
  .seg {{ flex:1; display:flex; background:#f1f5f9; border:2px solid #e2e8f0; border-radius:20px;
    padding:6px; gap:6px; }}
  .seg .opt {{ flex:1; height:76px; border-radius:15px; display:flex; align-items:center;
    justify-content:center; gap:12px; font-size:30px; font-weight:700; color:#64748b; }}
  .seg .opt svg {{ width:36px; height:36px; display:block; }}
  .seg .opt.on {{ background:#16a34a; color:#fff; box-shadow:0 6px 14px rgba(22,163,74,.3); }}
  .seg .opt.on svg {{ color:#fff; }}
  .seg .opt.on-no {{ background:#fff; color:#334155; box-shadow:0 4px 10px rgba(15,23,42,.1); }}
  /* request to join */
  .join {{ flex:1; height:88px; border-radius:20px; background:#1d4ed8; color:#fff;
    display:flex; align-items:center; justify-content:center; gap:14px;
    font-size:31px; font-weight:700; box-shadow:0 10px 22px rgba(29,78,216,.32); }}
  .join svg {{ width:40px; height:40px; color:#fff; }}
  /* subscribe = heart */
  .heartbtn {{ flex-shrink:0; align-self:flex-start; width:78px; height:78px; border-radius:20px;
    background:#fff; border:2px solid #cbd5e1; display:flex; align-items:center; justify-content:center; }}
  .heartbtn svg {{ width:44px; height:44px; color:#94a3b8; display:block; }}
  .heartbtn.on {{ background:#fee2e2; border-color:#fecaca; }}
  .heartbtn.on svg {{ color:#ef4444; }}

  nav {{ height:150px; background:#fff; border-top:2px solid #e7ebf0;
    display:flex; align-items:center; justify-content:space-around; padding-bottom:16px; }}
  nav .item {{ display:flex; flex-direction:column; align-items:center; gap:8px;
    font-size:24px; font-weight:700; color:#94a3b8; }}
  nav .item.active {{ color:#16a34a; }}
  nav .item svg {{ width:44px; height:44px; }}
</style></head><body>
  <div class="statusbar"><span>9:41</span><span class="dots">&#9679;&#9679;&#9679; &#9723;</span></div>
  <header><h1>Search games</h1></header>

  <div class="filters">
    <div class="searchbar">
      <span class="si">{I['icon-search']}</span>
      <span class="q ph">Search games</span>
    </div>
    <div class="chips">
      <div class="filterbtn">{I['icon-filter']}</div>
      <div class="chip heartchip"><span class="hh">{I['icon-heart']}</span>{I['icon-chevron-down']}</div>
      {chips}
    </div>
  </div>

  <div class="rescount">3 games found</div>

  <div class="list">
    {cards}
  </div>

  <nav>
    <div class="item">{I['icon-players']}<span>Agenda</span></div>
    <div class="item active">{I['icon-search']}<span>Search</span></div>
    <div class="item">{I['icon-bell']}<span>Alerts</span></div>
    <div class="item">{I['icon-pin']}<span>Nearby</span></div>
  </nav>
</body></html>'''

html_path = ROOT / "search-games-mockup.html"
html_path.write_text(HTML)
print("wrote", html_path)

out_png = ROOT / "search-games-mockup.png"
subprocess.run([
    CHROMIUM, "--headless=new", "--no-sandbox", "--hide-scrollbars",
    "--force-device-scale-factor=1", "--window-size=1080,1920",
    f"--screenshot={out_png}", f"file://{html_path}",
], check=True, capture_output=True)
print("rendered", out_png)

for name, svg in I.items():
    wrap = f'''<!doctype html><html><head><meta charset="utf-8"><style>
      *{{margin:0;padding:0}} html,body{{width:200px;height:200px}}
      .b{{width:200px;height:200px;display:flex;align-items:center;justify-content:center;color:#334155}}
      .b svg{{width:150px;height:150px}}</style></head><body><div class="b">{svg}</div></body></html>'''
    wp = PNG_DIR / f"{name}.html"
    wp.write_text(wrap)
    subprocess.run([
        CHROMIUM, "--headless=new", "--no-sandbox", "--hide-scrollbars",
        "--force-device-scale-factor=1", "--window-size=200,200",
        "--default-background-color=00000000",
        f"--screenshot={PNG_DIR / (name + '.png')}", f"file://{wp}",
    ], check=True, capture_output=True)
    wp.unlink()
print("exported", len(I), "icon PNGs")
