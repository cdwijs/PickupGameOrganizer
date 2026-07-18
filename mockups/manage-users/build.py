#!/usr/bin/env python3
"""Build the PickupGameOrganizer "Manage members" mockup for Terrible Football
Haarlem.

Top "me" card (Cedric) with a disabled "Request to become admin" button and a
"Leave group" button, then members grouped Requests -> Admins -> Members. Each
row has a remove control, and either an Approve action (requests), an Admin
badge (admins), or a Make-admin promote action (members). Reuses the shared icon
set; renders to a full-HD (1080x1920) portrait PNG with headless Chromium and
exports a transparent PNG of each icon. Source of truth: assets/svg/.
"""
import subprocess
import pathlib

ROOT = pathlib.Path(__file__).parent
SVG_DIR = ROOT / "assets" / "svg"
PNG_DIR = ROOT / "assets" / "png"
CHROMIUM = "chromium"

I = {p.stem: p.read_text() for p in SVG_DIR.glob("*.svg")}

# name, initial, avatar colour, role, me
USERS = [
    dict(name="Piet",   initial="P", color="#0ea5e9", role="request"),
    dict(name="Tim",    initial="T", color="#8b5cf6", role="request"),
    dict(name="Jan",    initial="J", color="#f59e0b", role="admin"),
    dict(name="Cedric", initial="C", color="#1d4ed8", role="admin", me=True),
    dict(name="Klaas",  initial="K", color="#14b8a6", role="member"),
]

SECTIONS = [
    ("request", "Requests", "#f59e0b"),
    ("admin", "Administrators", "#4f46e5"),
    ("member", "Members", "#64748b"),
]

SUB = {"request": "Wants to join", "admin": "Administrator", "member": "Member"}


def row(u):
    me = u.get("me")
    sub = SUB[u["role"]] + (" &middot; You" if me else "")

    # admin shield inline, right after the name, at name height
    shield = f' <span class="shield">{I["icon-admin"]}</span>' if u["role"] == "admin" else ""

    # right action per role (admins no longer carry a right-side pill)
    if u["role"] == "request":
        right = f'<div class="act approve">{I["icon-tick"]}<span>Approve</span></div>'
    elif u["role"] == "member":
        right = f'<div class="act promote">{I["icon-promote"]}<span>Make admin</span></div>'
    else:
        right = ""

    # delete control (not for yourself)
    delete = (f'<div class="del">{I["icon-trash"]}</div>' if not me
              else '<div class="del ghost"></div>')

    return f'''
      <div class="row">
        <div class="ava" style="background:{u['color']}">{u['initial']}</div>
        <div class="who"><div class="nm">{u['name']}{shield}</div><div class="sb">{sub}</div></div>
        {right}
        {delete}
        <div class="chat">{I['icon-chat']}</div>
      </div>'''


def section(role, title, color):
    users = [u for u in USERS if u["role"] == role]
    rows = "\n".join(row(u) for u in users)
    return f'''
    <div class="sec">
      <div class="sechead"><span class="sectitle">{title}</span>
        <span class="count" style="background:{color}">{len(users)}</span></div>
      {rows}
    </div>'''


sections = "\n".join(section(*s) for s in SECTIONS)

HTML = f'''<!doctype html><html><head><meta charset="utf-8"><style>
  * {{ margin:0; padding:0; box-sizing:border-box; }}
  html,body {{ width:1080px; height:1920px; overflow:hidden; }}
  body {{ font-family:'DejaVu Sans','Liberation Sans',sans-serif;
    background:#eef1f5; color:#0f172a; display:flex; flex-direction:column; }}

  .statusbar {{ height:56px; display:flex; align-items:center; justify-content:space-between;
    padding:0 48px; font-size:26px; font-weight:700; }}
  .statusbar .dots {{ letter-spacing:3px; }}

  .appbar {{ display:flex; align-items:center; gap:26px; padding:14px 44px 4px; }}
  .appbar .back {{ width:74px; height:74px; border-radius:22px; background:#fff;
    border:2px solid #e2e8f0; display:flex; align-items:center; justify-content:center; }}
  .appbar .back svg {{ width:40px; height:40px; color:#334155; }}
  .appbar h1 {{ font-size:50px; font-weight:700; letter-spacing:-.5px; }}
  .lead {{ padding:0 48px 8px; font-size:29px; font-weight:600; color:#64748b; }}

  .wrap {{ flex:1; padding:18px 48px 30px; display:flex; flex-direction:column; gap:42px;
    overflow:hidden; }}

  /* me card */
  .mecard {{ background:#fff; border:2px solid #e6ebf1; border-radius:30px; padding:40px 38px;
    box-shadow:0 14px 34px rgba(15,23,42,.06); }}
  .metop {{ display:flex; align-items:center; gap:24px; }}
  .meava {{ width:104px; height:104px; border-radius:50%; background:#1d4ed8; color:#fff;
    display:flex; align-items:center; justify-content:center; font-size:46px; font-weight:700;
    flex-shrink:0; box-shadow:0 8px 20px rgba(29,78,216,.28); }}
  .mename {{ flex:1; }}
  .mename .n {{ font-size:44px; font-weight:700; }}
  .mename .r {{ font-size:29px; font-weight:600; color:#94a3b8; margin-top:2px;
    display:flex; align-items:center; gap:10px; }}
  .mename .r .pill {{ background:#eef2ff; color:#4f46e5; font-size:24px; font-weight:700;
    padding:6px 16px; border-radius:999px; }}
  .mebtns {{ display:flex; gap:18px; margin-top:30px; }}
  .btn {{ flex:1; height:92px; border-radius:22px; display:flex; align-items:center;
    justify-content:center; gap:14px; font-size:30px; font-weight:700; }}
  .btn.disabled {{ background:#f1f5f9; color:#aab4c2; border:2px solid #e2e8f0; }}
  .btn.disabled svg {{ width:38px; height:38px; color:#aab4c2; }}
  .btn.giveup {{ background:#fff7ed; color:#c2660c; border:2px solid #fed7aa; }}
  .btn.giveup svg {{ width:38px; height:38px; color:#ea9214; }}
  .btn.leave {{ background:#fff; color:#ef4444; border:2px solid #fecaca; }}
  .btn.leave svg {{ width:38px; height:38px; color:#ef4444; }}

  /* sections */
  .sec {{ display:flex; flex-direction:column; gap:22px; }}
  .sechead {{ display:flex; align-items:center; gap:14px; padding:0 6px; }}
  .sectitle {{ font-size:27px; font-weight:700; color:#64748b; text-transform:uppercase;
    letter-spacing:1.5px; }}
  .count {{ color:#fff; font-size:24px; font-weight:700; min-width:40px; height:40px;
    padding:0 12px; border-radius:999px; display:flex; align-items:center; justify-content:center; }}

  .row {{ background:#fff; border:2px solid #e6ebf1; border-radius:26px; padding:30px 26px;
    display:flex; align-items:center; gap:22px; box-shadow:0 8px 22px rgba(15,23,42,.05); }}
  .del {{ width:70px; height:70px; border-radius:18px; background:#fef2f2; border:2px solid #fecaca;
    display:flex; align-items:center; justify-content:center; flex-shrink:0; }}
  .del svg {{ width:42px; height:42px; color:#ef4444; }}
  .del.ghost {{ background:transparent; border:2px solid transparent; }}
  .ava {{ width:92px; height:92px; border-radius:50%; color:#fff; flex-shrink:0;
    display:flex; align-items:center; justify-content:center; font-size:38px; font-weight:700; }}
  .who {{ flex:1; min-width:0; }}
  .who .nm {{ font-size:38px; font-weight:700; display:flex; align-items:center; gap:12px; }}
  .who .nm .shield {{ display:inline-flex; }}
  .who .nm .shield svg {{ width:36px; height:36px; color:#ea9214; display:block; }}
  .who .sb {{ font-size:26px; font-weight:600; color:#94a3b8; margin-top:2px; }}

  .chat {{ width:66px; height:66px; border-radius:18px; background:#eef2f7; border:2px solid #e2e8f0;
    display:flex; align-items:center; justify-content:center; flex-shrink:0; }}
  .chat svg {{ width:40px; height:40px; color:#475569; }}

  .act {{ flex-shrink:0; display:flex; align-items:center; gap:12px; height:66px; padding:0 26px;
    border-radius:18px; font-size:28px; font-weight:700; }}
  .act svg {{ width:38px; height:38px; display:block; }}
  .act.approve {{ background:#16a34a; color:#fff; box-shadow:0 8px 18px rgba(22,163,74,.3); }}
  .act.approve svg {{ color:#fff; }}
  .act.promote {{ background:#eef2ff; color:#1d4ed8; border:2px solid #c7d2fe; }}
  .act.promote svg {{ color:#1d4ed8; }}
  .act.badge {{ background:#fff7ed; color:#c2660c; border:2px solid #fed7aa; }}
  .act.badge svg {{ color:#ea9214; }}

  .qrbtn {{ height:100px; border-radius:24px; background:#1d4ed8; color:#fff;
    display:flex; align-items:center; justify-content:center; gap:18px;
    font-size:32px; font-weight:700; box-shadow:0 12px 26px rgba(29,78,216,.32); }}
  .qrbtn svg {{ width:46px; height:46px; color:#fff; }}
</style></head><body>
  <div class="statusbar"><span>9:41</span><span class="dots">&#9679;&#9679;&#9679; &#9723;</span></div>

  <div class="appbar">
    <div class="back">{I['icon-arrow-left']}</div>
    <h1>Members</h1>
  </div>
  <div class="lead">Terrible Football Haarlem &middot; 5 people</div>

  <div class="wrap">
    <div class="mecard">
      <div class="metop">
        <div class="meava">C</div>
        <div class="mename">
          <div class="n">Cedric</div>
          <div class="r">You <span class="pill">Administrator</span></div>
        </div>
        <div class="chat">{I['icon-chat']}</div>
      </div>
      <div class="mebtns">
        <div class="btn giveup">{I['icon-demote']}<span>Give up admin</span></div>
        <div class="btn leave">{I['icon-remove']}<span>Leave group</span></div>
      </div>
    </div>

    <div class="qrbtn">{I['icon-qr']}<span>Show invite QR code</span></div>

    {sections}
  </div>
</body></html>'''

html_path = ROOT / "manage-users-mockup.html"
html_path.write_text(HTML)
print("wrote", html_path)

out_png = ROOT / "manage-users-mockup.png"
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
