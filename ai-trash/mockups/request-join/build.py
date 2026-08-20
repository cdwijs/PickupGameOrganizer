#!/usr/bin/env python3
"""Build the PickupGameOrganizer "Request to join" mockup.

Group name at the top, a scrollable rules panel with an EN/NL language toggle,
and an "I agree" checkbox + "Request to join" button at the bottom. Reuses the
shared icon set from the other mockups; renders to a full-HD (1080x1920) portrait
PNG with headless Chromium and exports a transparent PNG of each icon. Source of
truth: assets/svg/.
"""
import subprocess
import pathlib

ROOT = pathlib.Path(__file__).parent
SVG_DIR = ROOT / "assets" / "svg"
PNG_DIR = ROOT / "assets" / "png"
CHROMIUM = "chromium"

I = {p.stem: p.read_text() for p in SVG_DIR.glob("*.svg")}

GROUP = "Terrible Football Haarlem"

RULES = [
    "The football club has gracefully given permission to use their field for free. "
    "Therefore we don't have access to the showers.",
    "We are also not allowed to bring our own consumptions (water is permitted).",
    "We are all amateurs, and we all make mistakes. Don't take it personal if somebody "
    "makes a mistake. If you do take it personal, go off the field, take care of business "
    "there, so the game can continue. Afterwards you are welcome to rejoin the game.",
    "Participation is at your own risk. We do try to keep each other in one piece, but "
    "accidents can happen.",
    "We have a limit of 30 people on the field. More people will hinder each other.",
    "In the winter, we are using the floodlights. The costs will be divided by the players, "
    "and the bill arrives via a Tikkie.",
    "We own yellow bibs. You are obligated to wash these bibs every once in a while.",
]

rules_html = "\n".join(
    f'<div class="rule"><div class="num">{i}</div><div class="rtext">{r}</div></div>'
    for i, r in enumerate(RULES, 1)
)

HTML = f'''<!doctype html><html><head><meta charset="utf-8"><style>
  * {{ margin:0; padding:0; box-sizing:border-box; }}
  html,body {{ width:1080px; height:1920px; overflow:hidden; }}
  body {{ font-family:'DejaVu Sans','Liberation Sans',sans-serif;
    background:#eef1f5; color:#0f172a; display:flex; flex-direction:column; }}

  .statusbar {{ height:56px; display:flex; align-items:center; justify-content:space-between;
    padding:0 48px; font-size:26px; font-weight:700; }}
  .statusbar .dots {{ letter-spacing:3px; }}

  .appbar {{ display:flex; align-items:center; gap:26px; padding:12px 44px 4px; }}
  .appbar .back {{ width:74px; height:74px; border-radius:22px; background:#fff;
    border:2px solid #e2e8f0; display:flex; align-items:center; justify-content:center; }}
  .appbar .back svg {{ width:40px; height:40px; color:#334155; }}
  .appbar h1 {{ font-size:44px; font-weight:700; letter-spacing:-.5px; }}

  /* group header */
  .ghead {{ display:flex; align-items:center; gap:26px; padding:12px 48px 6px; }}
  .gava {{ width:110px; height:110px; border-radius:50%; background:#1d4ed8; flex-shrink:0;
    display:flex; align-items:center; justify-content:center;
    box-shadow:0 10px 22px rgba(29,78,216,.28); }}
  .gava svg {{ width:64px; height:64px; color:#fff; }}
  .gn {{ font-size:46px; font-weight:700; letter-spacing:-.5px; line-height:1.1; }}
  .gs {{ font-size:29px; font-weight:600; color:#64748b; margin-top:6px; }}

  /* rules header + language toggle */
  .ruleshead {{ display:flex; align-items:center; justify-content:space-between;
    padding:20px 50px 12px; }}
  .rh {{ display:flex; align-items:center; gap:14px; font-size:29px; font-weight:700;
    color:#64748b; text-transform:uppercase; letter-spacing:1.5px; }}
  .rh svg {{ width:38px; height:38px; color:#94a3b8; }}
  .lang {{ display:flex; align-items:center; gap:16px; }}
  .lang .lg svg {{ width:40px; height:40px; color:#94a3b8; display:block; }}
  .seg2 {{ display:flex; background:#e7ebf1; border-radius:16px; padding:5px; gap:5px; }}
  .seg2 .opt {{ padding:12px 26px; border-radius:12px; font-size:28px; font-weight:700;
    color:#64748b; }}
  .seg2 .opt.on {{ background:#1d4ed8; color:#fff; box-shadow:0 4px 12px rgba(29,78,216,.3); }}

  /* rules panel with scrollbar */
  .rules {{ flex:1; position:relative; margin:0 48px; background:#fff; border:2px solid #e6ebf1;
    border-radius:28px; overflow:hidden; box-shadow:0 12px 30px rgba(15,23,42,.06); }}
  .content {{ position:absolute; inset:0; padding:38px 96px 38px 40px; }}
  .rule {{ display:flex; gap:24px; margin-bottom:34px; }}
  .num {{ width:56px; height:56px; border-radius:50%; background:#eef2ff; color:#1d4ed8;
    font-size:30px; font-weight:700; flex-shrink:0;
    display:flex; align-items:center; justify-content:center; }}
  .rtext {{ font-size:33px; line-height:1.5; font-weight:500; color:#1f2937; padding-top:4px; }}
  .fade {{ position:absolute; left:2px; right:2px; bottom:2px; height:90px; border-radius:0 0 26px 26px;
    background:linear-gradient(to bottom, rgba(255,255,255,0), #fff); }}
  .track {{ position:absolute; top:24px; bottom:24px; right:22px; width:12px; border-radius:999px;
    background:#e2e8f0; }}
  .thumb {{ position:absolute; top:24px; right:20px; width:16px; height:300px; border-radius:999px;
    background:#94a3b8; }}

  /* footer: agree + request */
  footer {{ padding:24px 48px 40px; }}
  .agree {{ display:flex; align-items:center; gap:22px; padding:6px 4px 0; }}
  .box {{ width:60px; height:60px; border-radius:16px; background:#16a34a; flex-shrink:0;
    display:flex; align-items:center; justify-content:center; }}
  .box svg {{ width:40px; height:40px; color:#fff; }}
  .agree .at {{ font-size:33px; font-weight:700; color:#0f172a; }}
  .reqbtn {{ height:108px; border-radius:26px; background:#1d4ed8; color:#fff; margin-top:26px;
    display:flex; align-items:center; justify-content:center; gap:18px;
    font-size:38px; font-weight:700; box-shadow:0 14px 30px rgba(29,78,216,.35); }}
  .reqbtn svg {{ width:46px; height:46px; color:#fff; }}
</style></head><body>
  <div class="statusbar"><span>9:41</span><span class="dots">&#9679;&#9679;&#9679; &#9723;</span></div>

  <div class="appbar">
    <div class="back">{I['icon-arrow-left']}</div>
    <h1>Request to join</h1>
  </div>

  <div class="ghead">
    <div class="gava">{I['icon-group-football']}</div>
    <div class="gt"><div class="gn">{GROUP}</div><div class="gs">Please read and accept the group rules</div></div>
  </div>

  <div class="ruleshead">
    <div class="rh">{I['icon-doc']}<span>Group rules</span></div>
    <div class="lang">
      <span class="lg">{I['icon-website']}</span>
      <div class="seg2"><div class="opt on">EN</div><div class="opt">NL</div></div>
    </div>
  </div>

  <div class="rules">
    <div class="content">
      {rules_html}
    </div>
    <div class="fade"></div>
    <div class="track"></div>
    <div class="thumb"></div>
  </div>

  <footer>
    <div class="agree">
      <div class="box">{I['icon-tick']}</div>
      <span class="at">I agree to the group rules</span>
    </div>
    <div class="reqbtn">{I['icon-join']}<span>Request to join</span></div>
  </footer>
</body></html>'''

html_path = ROOT / "request-join-mockup.html"
html_path.write_text(HTML)
print("wrote", html_path)

out_png = ROOT / "request-join-mockup.png"
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
