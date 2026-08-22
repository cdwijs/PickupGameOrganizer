# prototype-OSM

A PWA that asks where you are, rounds that to a tenth of a degree, and offers
to download the OpenStreetMap data for the spot.

Four panels, in order:

1. **Where you are** — the browser's permission prompt, then latitude and
   longitude in decimal degrees with the reported accuracy.
2. **Rounded to 0.1°** — the two numbers, and the 0.1° cell they name.
3. **OSM map data** — a box centred on the rounded pair, fetched as raw `.osm`
   XML and saved as a file.
4. **Map** — that same XML drawn on a canvas, so you can see what you got.

No build step, no dependency, no key. Everything is `index.html` + `app.js`,
the same shape as the other prototypes here.

## The two numbers

`roundTenth()` rounds half **away from zero**, so the grid falls the same way
north and south of the equator — plain `Math.round` sends `-0.05` to `-0` and
`0.05` to `0.1`, which would put the boundary in a different place either side
of it. The value is formatted to exactly one decimal, so a fix of

```
52.370216 °N   4.895168 °E
```

reads as **52.4 °N, 4.9 °E**. That pair names a cell running 52.35°–52.45° and
4.85°–4.95°: about 11.1 km north-south by 6.8 km east-west at this latitude.

## What the download actually fetches

The endpoint is the OpenStreetMap API's `/api/0.6/map?bbox=…`, the same call
JOSM makes, returning raw OSM XML. It refuses anything over **0.25 square
degrees or 50 000 nodes**, whichever comes first, and the node limit is the one
that bites. Measured against the live API:

| Request | Result |
|---|---|
| 0.1° cell, rural | **400** — "You requested too many nodes (limit is 50000)" |
| 0.05° box, city | **400** — same |
| 0.02° box, city | **400** — same |
| 0.03° box, rural | 200 — 8.0 MB |
| **0.01° box, city** | 200 — 13.4 MB in 2.0 s |
| 0.01° box, rural | 200 — 1.2 MB |
| 0.005° box, city | 200 — 4.3 MB |

So the `/map` endpoint cannot serve a whole 0.1° cell **anywhere** — not even
over farmland. The only route that can is Overpass, and a 0.1° rural cell
measured there came back as **219 MB in 42 seconds**, which is not something to
hand a phone, or to ask a volunteer service for from a prototype.

The download is therefore **capped at 0.01°** — roughly 1.1 km × 0.7 km, a
couple of megabytes — with 0.005° available for somewhere dense enough that
even that is refused. When the API does refuse, the app repeats its reason and
points at the smaller size rather than reporting a generic failure.

### The box is centred on the rounded pair

Which means it usually does **not** contain you: rounding moves the centre by
up to 0.05°, about 5.6 km. The panel says so — either *"you are inside it,
156 m from its centre"* or *"3.3 km from where you are, so your own position is
outside it"* — so the offset is visible before you download rather than a
surprise in JOSM afterwards.

That is what "the map data for that location" means here: the location is the
rounded pair, not the raw fix. Centring on the raw position instead is a
one-line change in `download()`.

## The map window

The download is rendered from the bytes already in hand — no second request, no
tile service, no library. Tiles are somebody else's servers with their own
usage policy, and drawing the data is the more useful thing anyway: the picture
*is* the download, so a wrong bbox or an empty area is visible immediately.

Ways are sorted into layers — green, water, buildings, paths, rail, streets,
main roads — and painted in that order, with road widths scaled in metres of
ground so zooming in widens them the way a map should. There is a scale bar, a
dot for your position when it is on screen, and a dashed rectangle showing the
box that was requested.

**The data reaches past that rectangle**, because the API returns whole ways: a
street clipped by the bbox still arrives complete, and a motorway can carry on
for kilometres. The view frames the requested box with 12 % of air around it
and lets the surplus run off the edges. Framing the data extent instead was
tried and is worse — one long way drops the interesting part to a tenth of the
canvas.

Drag to pan, scroll or pinch to zoom, and the three buttons do zoom in, zoom
out and fit.

What is **not** drawn: relations, so a multipolygon lake appears as its outline
rather than a filled shape; standalone POI nodes; and any labels at all. Adding
multipolygon assembly is the obvious next step if this needs to look like a
real map.

## Running it

```sh
cd ai-trash/prototypes/prototype-OSM
python3 -m http.server 8080
# then open http://localhost:8080 in a modern browser
```

`http://localhost` counts as a secure context, so geolocation and the service
worker both work there.

**On a phone this must be HTTPS.** `http://<lan-ip>:8080` gives no geolocation
at all — the app detects that and says so instead of letting the request fail
as "position unavailable". The LAN certificate setup is the same one
`prototype-qr-scanner/` documents; see
[`../prototype-qr-scanner/README.md`](../prototype-qr-scanner/README.md) under
*Option 1*, and trust the certificate rather than clicking through the warning
(a bypassed interstitial leaves the origin flagged and kills the service
worker).

## What to try

1. Open it with location permission already granted — it asks for a fix
   immediately, without a second tap. On a fresh visit it waits for the button,
   because an unprompted request is what browsers throttle.
2. Refuse the permission. The pill reads *refused* and the note explains that
   the browser will not ask again by itself.
3. Watch the rounded pair against the raw reading — 52.370216 becomes 52.4.
4. Check the **centred on** line before downloading, to see how far the box has
   moved from you.
5. Press **Download map data**. The pill counts the bytes as they arrive and
   the file lands as `osm-52.4N-4.9E-0.01deg.osm`.
6. Open the file: it is XML with a `<bounds>` element matching the bbox that
   was requested.
7. Look at the **Map** panel that appears under it, and compare the dashed
   rectangle with how far the data actually extends. Pinch to zoom in until the
   buildings separate.
8. Pull the network cable and reload. The shell still loads; the download
   obviously does not.

## Files

- `index.html` — markup, styling, the four panels.
- `app.js` — geolocation, rounding, bbox maths, the fetch with progress and
  cancel, the file save, and the canvas renderer (XML parse, layer
  classification, projection, pan and zoom).
- `manifest.json` — PWA manifest; makes the page installable.
- `sw.js` — network-first service worker over the app shell, the same strategy
  `prototype-minimal` uses. API calls are cross-origin and pass straight
  through it; a cached map extract would be both useless and enormous.
- `icon.svg` — a pin on a grid.

## Notes and limitations

- The response is buffered in memory as a Blob before the browser can save it.
  At the capped size that is a few megabytes; it is the reason the cap matters
  on a phone. Streaming straight to disk needs the File System Access API,
  which Gecko does not implement.
- A download can be cancelled mid-flight — `AbortController` — and the byte
  counter moves while it runs. The progress *bar* only appears when the server
  sent a `Content-Length`, which these endpoints usually do not.
- Nothing is persisted: no position in `localStorage`, no history, nothing
    kept from the render. The app holds the current fix and the parsed geometry
    in memory and forgets both on reload.
- The bbox is clamped to ±90 / ±180, so a cell against a pole or the
  antimeridian is clipped rather than sent as an invalid request. A cell that
  straddles the antimeridian is clipped, not split into two.
- The XML is parsed with `DOMParser` on the main thread, so a dense extract
  stalls the page for a moment while it renders. The pill says *rendering…*
  first. A worker would fix it and is not worth it at this size.
- The projection is equirectangular scaled by the cosine of the centre
  latitude. Over a box a kilometre across the difference from Mercator is far
  below a pixel; over a whole cell it would not be.
- Data © OpenStreetMap contributors, ODbL. Anything downloaded here carries
  that licence with it, and so does anything drawn from it.
