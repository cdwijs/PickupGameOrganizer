#!/usr/bin/env python3
"""Build the PickupGameOrganizer "Create new group" form mockup.

Composes the shared icon set (reused from the agenda mockup) plus a few
form-specific icons into a full-HD (1080x1920) portrait HTML layout, renders it
to PNG with headless Chromium, and exports a transparent PNG of each icon.
Single source of truth: the SVG files in assets/svg/.
"""
import subprocess
import pathlib

ROOT = pathlib.Path(__file__).parent
SVG_DIR = ROOT / "assets" / "svg"
PNG_DIR = ROOT / "assets" / "png"
CHROMIUM = "chromium"

I = {p.stem: p.read_text() for p in SVG_DIR.glob("*.svg")}


def field(icon, label, value, placeholder=False, sub=None):
    cls = "val ph" if placeholder else "val"
    return f'''
      <div class="field">
        <div class="lbl">{label}</div>
        <div class="input">
          <span class="ico">{I[icon]}</span>
          <span class="{cls}">{value}</span>
        </div>
        {f'<div class="hint">{sub}</div>' if sub else ''}
      </div>'''


HTML = f'''<!doctype html><html><head><meta charset="utf-8"><style>
  * {{ margin:0; padding:0; box-sizing:border-box; }}
  html,body {{ width:1080px; height:1920px; overflow:hidden; }}
  body {{ font-family:'DejaVu Sans','Liberation Sans',sans-serif;
    background:#eef1f5; color:#0f172a; display:flex; flex-direction:column; }}

  .statusbar {{ height:56px; display:flex; align-items:center; justify-content:space-between;
    padding:0 48px; font-size:26px; font-weight:700; }}
  .statusbar .dots {{ letter-spacing:3px; }}

  .appbar {{ display:flex; align-items:center; gap:26px; padding:14px 44px 10px; }}
  .appbar .back {{ width:74px; height:74px; border-radius:22px; background:#fff;
    border:2px solid #e2e8f0; display:flex; align-items:center; justify-content:center; }}
  .appbar .back svg {{ width:40px; height:40px; color:#334155; }}
  .appbar h1 {{ font-size:52px; font-weight:700; letter-spacing:-.5px; }}

  .intro {{ display:flex; align-items:center; gap:24px; padding:8px 48px 2px; }}
  .intro .avatar {{ width:96px; height:96px; border-radius:50%; background:#1d4ed8;
    flex-shrink:0; display:flex; align-items:center; justify-content:center;
    box-shadow:0 10px 22px rgba(29,78,216,.28); }}
  .intro .avatar svg {{ width:54px; height:54px; color:#fff; }}
  .intro .t {{ font-size:31px; font-weight:700; color:#334155; line-height:1.25; }}
  .intro .t span {{ display:block; font-size:26px; font-weight:600; color:#94a3b8; }}

  .form {{ flex:1; padding:16px 48px 6px; display:flex; flex-direction:column; gap:13px;
    overflow:hidden; }}
  .field .lbl {{ font-size:25px; font-weight:700; color:#64748b; text-transform:uppercase;
    letter-spacing:1px; margin:0 6px 7px; }}
  .input {{ background:#fff; border:2px solid #e2e8f0; border-radius:22px;
    height:80px; display:flex; align-items:center; gap:22px; padding:0 28px; }}
  .input .ico {{ flex-shrink:0; }}
  .input .ico svg {{ width:42px; height:42px; color:#1d4ed8; display:block; }}
  .val {{ font-size:34px; font-weight:600; color:#0f172a; white-space:nowrap;
    overflow:hidden; text-overflow:ellipsis; flex:1; min-width:0; }}
  .val.ph {{ color:#a3adba; font-weight:500; }}
  .hint {{ font-size:23px; color:#94a3b8; margin:8px 6px 0; }}

  /* grouped costs card */
  .group-card {{ background:#fff; border:2px solid #e2e8f0; border-radius:26px; padding:22px 26px 22px; }}
  .group-card .lbl {{ font-size:25px; font-weight:700; color:#64748b; text-transform:uppercase;
    letter-spacing:1px; margin:0 4px 16px; }}
  .group-card .input {{ margin-bottom:16px; }}
  .group-card .input:last-of-type {{ margin-bottom:0; }}
  .check {{ display:flex; align-items:center; gap:20px; margin-top:20px; padding:0 4px; }}
  .box {{ width:52px; height:52px; border-radius:14px; background:#16a34a; flex-shrink:0;
    display:flex; align-items:center; justify-content:center; }}
  .box svg {{ width:36px; height:36px; color:#fff; }}
  .check .ctxt {{ font-size:30px; font-weight:600; color:#334155; }}
  .check .ctxt span {{ color:#94a3b8; font-weight:500; }}

  /* inline "Set" button inside the location field */
  .input.loc {{ padding-right:12px; }}
  .caret {{ flex-shrink:0; }}
  .caret svg {{ width:40px; height:40px; color:#94a3b8; display:block; }}
  .setbtn {{ flex-shrink:0; display:flex; align-items:center; gap:10px; height:56px;
    padding:0 26px; border-radius:16px; background:#1d4ed8; color:#fff;
    font-size:29px; font-weight:700; }}
  .setbtn svg {{ width:34px; height:34px; color:#fff; }}

  /* seasonal pricing */
  .seasons {{ display:flex; gap:16px; margin-bottom:16px; }}
  .season {{ flex:1; border:2px solid #e2e8f0; border-radius:18px; padding:18px 20px;
    display:flex; align-items:center; gap:16px; background:#f8fafc; }}
  .season .sico svg {{ width:44px; height:44px; display:block; }}
  .season.summer .sico svg {{ color:#f59e0b; }}
  .season.winter .sico svg {{ color:#0ea5e9; }}
  .season .sname {{ font-size:24px; font-weight:700; color:#64748b; text-transform:uppercase; letter-spacing:.5px; }}
  .season .sval {{ font-size:30px; font-weight:700; color:#0f172a; margin-top:2px; }}
  .season.summer .sval {{ color:#16a34a; }}
  .cap {{ font-size:23px; font-weight:700; color:#94a3b8; text-transform:uppercase;
    letter-spacing:.5px; margin:2px 6px 8px; }}
  .divider {{ height:2px; background:#eef1f5; margin:16px 0 14px; }}

  /* two column row */
  .row2 {{ display:flex; gap:26px; }}
  .row2 .field {{ flex:1; }}
  .stepper {{ background:#fff; border:2px solid #e2e8f0; border-radius:22px; height:80px;
    display:flex; align-items:center; justify-content:space-between; padding:0 18px; }}
  .stepper .btn {{ width:56px; height:56px; border-radius:16px; background:#f1f5f9;
    display:flex; align-items:center; justify-content:center; }}
  .stepper .btn svg {{ width:34px; height:34px; color:#334155; }}
  .stepper .num {{ display:flex; align-items:center; gap:14px; }}
  .stepper .num .pico svg {{ width:36px; height:36px; color:#1d4ed8; }}
  .stepper .num .n {{ font-size:38px; font-weight:700; }}

  footer {{ padding:12px 48px 26px; }}
  .create {{ height:104px; border-radius:26px; background:#16a34a; color:#fff;
    display:flex; align-items:center; justify-content:center; gap:18px;
    font-size:38px; font-weight:700; box-shadow:0 14px 30px rgba(22,163,74,.35); }}
  .create svg {{ width:46px; height:46px; color:#fff; }}
</style></head><body>
  <div class="statusbar"><span>9:41</span><span class="dots">&#9679;&#9679;&#9679; &#9723;</span></div>

  <div class="appbar">
    <div class="back">{I['icon-arrow-left']}</div>
    <h1>New group</h1>
  </div>

  <div class="form">
    {field('icon-group-football', 'Group name', 'Terrible Football Haarlem')}

    <div class="field">
      <div class="lbl">Sport</div>
      <div class="input">
        <span class="ico">{I['icon-group-football']}</span>
        <span class="val">Soccer</span>
        <span class="caret">{I['icon-chevron-down']}</span>
      </div>
    </div>

    {field('icon-players', 'Audience', 'Age 16+ &middot; Any gender')}

    <div class="field">
      <div class="lbl">Default location</div>
      <div class="input loc">
        <span class="ico">{I['icon-pin']}</span>
        <span class="val">Sportpark Haarlem-Oost, Haarlem</span>
        <div class="setbtn">{I['icon-target']}<span>Set</span></div>
      </div>
    </div>

    <div class="group-card">
      <div class="lbl">Costs</div>
      <div class="seasons">
        <div class="season summer">
          <span class="sico">{I['icon-sun']}</span>
          <div><div class="sname">Summer</div><div class="sval">Free</div></div>
        </div>
        <div class="season winter">
          <span class="sico">{I['icon-snow']}</span>
          <div><div class="sname">Winter</div><div class="sval">&euro; 2&ndash;5 &middot; lights</div></div>
        </div>
      </div>
      <div class="cap">Tikkie &mdash; game costs</div>
      <div class="input"><span class="ico">{I['icon-link']}</span><span class="val">https://tikkie.me/pay/tf-games</span></div>
      <div class="divider"></div>
      <div class="check">
        <div class="box">{I['icon-tick']}</div>
        <div class="ctxt">Also accept donations <span>&mdash; pay what you want</span></div>
      </div>
      <div class="cap" style="margin-top:16px">Tikkie &mdash; donations</div>
      <div class="input"><span class="ico">{I['icon-link']}</span><span class="val">https://tikkie.me/pay/tf-donations</span></div>
    </div>

    {field('icon-chat', 'Chat — upcoming game', 'Paste chat invite link', placeholder=True)}
    {field('icon-chat', 'Chat — off-topic', 'Paste chat invite link', placeholder=True)}
    {field('icon-doc', 'Link to rules', 'https://www.mellekoning.nl/tfootball/page.html')}

    <div class="row2">
      <div class="field">
        <div class="lbl">Min. players</div>
        <div class="stepper">
          <div class="btn">{I['icon-minus']}</div>
          <div class="num"><span class="pico">{I['icon-players']}</span><span class="n">8</span></div>
          <div class="btn">{I['icon-plus']}</div>
        </div>
      </div>
      <div class="field">
        <div class="lbl">Max. players</div>
        <div class="stepper">
          <div class="btn">{I['icon-minus']}</div>
          <div class="num"><span class="pico">{I['icon-players']}</span><span class="n">30</span></div>
          <div class="btn">{I['icon-plus']}</div>
        </div>
      </div>
    </div>
  </div>

  <footer>
    <div class="create">{I['icon-tick']}<span>Create group</span></div>
  </footer>
</body></html>'''

html_path = ROOT / "new-group-mockup.html"
html_path.write_text(HTML)
print("wrote", html_path)

out_png = ROOT / "new-group-mockup.png"
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
