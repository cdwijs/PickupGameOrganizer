#!/usr/bin/env python3
"""Build the PickupGameOrganizer "Schedule a new game" mockup.

Two recurring-schedule blocks (Monday & Friday), each with a delete button and a
location prefilled with the default location from the new-group mockup, plus an
"Add schedule" button at the bottom. Reuses the shared icon set from the other
mockups; renders to a full-HD (1080x1920) portrait PNG with headless Chromium
and exports a transparent PNG of each icon. Source of truth: assets/svg/.
"""
import subprocess
import pathlib

ROOT = pathlib.Path(__file__).parent
SVG_DIR = ROOT / "assets" / "svg"
PNG_DIR = ROOT / "assets" / "png"
CHROMIUM = "chromium"

I = {p.stem: p.read_text() for p in SVG_DIR.glob("*.svg")}

DEFAULT_LOCATION = "Sportpark Haarlem-Oost, Haarlem"

BLOCKS = [
    dict(n=1, badge="MON", color="#1d4ed8", day="Every Monday", start="19:00", end="21:00"),
    dict(n=2, badge="FRI", color="#7c3aed", day="Every Friday", start="19:00", end="21:00"),
]


def block(b):
    return f'''
    <div class="card">
      <div class="chead">
        <div class="badge" style="background:{b['color']}">{b['badge']}</div>
        <div class="ctitle">Weekly game <span>Schedule {b['n']}</span></div>
        <div class="del">{I['icon-trash']}</div>
      </div>
      <div class="sub">
        <div class="lbl">Schedule</div>
        <div class="input"><span class="ico">{I['icon-calendar']}</span><span class="val">{b['day']}</span><span class="caret">{I['icon-chevron-down']}</span></div>
        <div class="timerow">
          <div class="tcol">
            <div class="cap">Start</div>
            <div class="input"><span class="ico">{I['icon-clock']}</span><span class="val">{b['start']}</span><span class="caret">{I['icon-chevron-down']}</span></div>
          </div>
          <span class="dash">&ndash;</span>
          <div class="tcol">
            <div class="cap">End</div>
            <div class="input"><span class="ico">{I['icon-clock']}</span><span class="val">{b['end']}</span><span class="caret">{I['icon-chevron-down']}</span></div>
          </div>
        </div>
      </div>
      <div class="sub">
        <div class="lbl">Location</div>
        <div class="input loc">
          <span class="ico">{I['icon-pin']}</span>
          <span class="val">{DEFAULT_LOCATION}</span>
          <div class="setbtn">{I['icon-target']}<span>Set</span></div>
        </div>
      </div>
    </div>'''


cards = "\n".join(block(b) for b in BLOCKS)

HTML = f'''<!doctype html><html><head><meta charset="utf-8"><style>
  * {{ margin:0; padding:0; box-sizing:border-box; }}
  html,body {{ width:1080px; height:1920px; overflow:hidden; }}
  body {{ font-family:'DejaVu Sans','Liberation Sans',sans-serif;
    background:#eef1f5; color:#0f172a; display:flex; flex-direction:column; }}

  .statusbar {{ height:56px; display:flex; align-items:center; justify-content:space-between;
    padding:0 48px; font-size:26px; font-weight:700; }}
  .statusbar .dots {{ letter-spacing:3px; }}

  .appbar {{ display:flex; align-items:center; gap:26px; padding:14px 44px 6px; }}
  .appbar .back {{ width:74px; height:74px; border-radius:22px; background:#fff;
    border:2px solid #e2e8f0; display:flex; align-items:center; justify-content:center; }}
  .appbar .back svg {{ width:40px; height:40px; color:#334155; }}
  .appbar h1 {{ font-size:52px; font-weight:700; letter-spacing:-.5px; flex:1; }}
  .appbar .save {{ font-size:33px; font-weight:700; color:#16a34a; }}
  .lead {{ padding:6px 48px 10px; font-size:30px; font-weight:600; color:#64748b; }}

  .list {{ flex:1; padding:20px 48px 8px; display:flex; flex-direction:column; gap:36px;
    overflow:hidden; }}

  .card {{ background:#fff; border:2px solid #e6ebf1; border-radius:32px; padding:34px 38px;
    box-shadow:0 14px 34px rgba(15,23,42,.06); }}
  .chead {{ display:flex; align-items:center; gap:24px; margin-bottom:26px; }}
  .badge {{ width:88px; height:88px; border-radius:22px; color:#fff; flex-shrink:0;
    display:flex; align-items:center; justify-content:center; font-size:30px; font-weight:700;
    letter-spacing:1px; box-shadow:0 8px 18px rgba(29,78,216,.22); }}
  .ctitle {{ flex:1; font-size:40px; font-weight:700; line-height:1.1; }}
  .ctitle span {{ display:block; font-size:26px; font-weight:600; color:#94a3b8; margin-top:4px; }}
  .del {{ width:74px; height:74px; border-radius:20px; background:#fef2f2; border:2px solid #fecaca;
    display:flex; align-items:center; justify-content:center; flex-shrink:0; }}
  .del svg {{ width:42px; height:42px; color:#ef4444; }}

  .sub {{ margin-top:28px; }}
  .sub:first-of-type {{ margin-top:0; }}
  .lbl {{ font-size:25px; font-weight:700; color:#64748b; text-transform:uppercase;
    letter-spacing:1px; margin:0 6px 16px; }}

  .timerow {{ display:flex; align-items:flex-end; gap:20px; margin-top:18px; }}
  .timerow .tcol {{ flex:1; }}
  .cap {{ font-size:24px; font-weight:700; color:#94a3b8; text-transform:uppercase;
    letter-spacing:.5px; margin:0 6px 8px; }}
  .dash {{ font-size:34px; font-weight:700; color:#94a3b8; padding-bottom:24px; }}

  .input {{ background:#f8fafc; border:2px solid #e2e8f0; border-radius:22px; height:92px;
    display:flex; align-items:center; gap:20px; padding:0 26px; }}
  .input .ico {{ flex-shrink:0; }}
  .input .ico svg {{ width:42px; height:42px; color:#1d4ed8; display:block; }}
  .val {{ font-size:34px; font-weight:600; color:#0f172a; white-space:nowrap;
    overflow:hidden; text-overflow:ellipsis; flex:1; min-width:0; }}
  .caret {{ flex-shrink:0; }}
  .caret svg {{ width:38px; height:38px; color:#94a3b8; display:block; }}

  .input.loc {{ padding-right:12px; }}
  .setbtn {{ flex-shrink:0; display:flex; align-items:center; gap:10px; height:60px;
    padding:0 26px; border-radius:16px; background:#1d4ed8; color:#fff;
    font-size:29px; font-weight:700; }}
  .setbtn svg {{ width:34px; height:34px; color:#fff; }}

  .addbtn {{ height:108px; border-radius:26px; background:#eef2ff; border:3px dashed #93a9f5;
    display:flex; align-items:center; justify-content:center; gap:18px;
    font-size:34px; font-weight:700; color:#1d4ed8; margin-top:8px; }}
  .addbtn svg {{ width:46px; height:46px; color:#1d4ed8; }}

  footer {{ padding:14px 48px 40px; }}
  .save-btn {{ height:104px; border-radius:26px; background:#16a34a; color:#fff;
    display:flex; align-items:center; justify-content:center; gap:18px;
    font-size:38px; font-weight:700; box-shadow:0 14px 30px rgba(22,163,74,.35); }}
  .save-btn svg {{ width:46px; height:46px; color:#fff; }}
</style></head><body>
  <div class="statusbar"><span>9:41</span><span class="dots">&#9679;&#9679;&#9679; &#9723;</span></div>

  <div class="appbar">
    <div class="back">{I['icon-arrow-left']}</div>
    <h1>Schedule</h1>
    <span class="save">Save</span>
  </div>
  <div class="lead">Set up recurring games for your group</div>

  <div class="list">
    {cards}
    <div class="addbtn">{I['icon-plus']}<span>Add schedule</span></div>
  </div>

  <footer>
    <div class="save-btn">{I['icon-tick']}<span>Save schedule</span></div>
  </footer>
</body></html>'''

html_path = ROOT / "schedule-game-mockup.html"
html_path.write_text(HTML)
print("wrote", html_path)

out_png = ROOT / "schedule-game-mockup.png"
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
