# T-010 spike conclusions.
#
# Recorded as a file so the next session does not have to re-measure.

## Question 1: does a WS_EX_NOACTIVATE topmost window receive mouse input?

Yes. `spike_t010_hover --mode=overlay --auto-move`:

```
overlay=ready rect=400,400 600x160 hittest=opaque
track=requested ok=1
event=mousemove t=745 x=200 y=80
event=mousemove t=821 x=300 y=80
event=mousemove t=900 x=500 y=80
auto_move=done moves=3
event=mouseleave t=992
auto_leave=done leaves=1
overlay=summary moves=3 leaves=1 clicks=0
```

WM_MOUSEMOVE arrives without the window ever taking focus, TrackMouseEvent(TME_LEAVE)
returns 1, and WM_MOUSELEAVE arrives when the cursor leaves. Hover magnification has
working input on the dock's existing window style.

Note: five SetCursorPos calls produced three WM_MOUSEMOVE. Windows coalesces moves
that land in the same message pump cycle, which is exactly what the dock wants — one
layout evaluation per frame, not per hardware sample.

## Question 2: does HTTRANSPARENT pass a click to another process?

No. `spike_t010_hover --transparent` returns HTTRANSPARENT outside a 40px band:

```
sendinput=click at=700,420
overlay=summary moves=2 leaves=0 clicks=1
victim=done clicks=0
```

The click in the transparent band was swallowed by the overlay; the victim window
underneath received nothing. The SDK documents HTTRANSPARENT as "let the message
continue to the underlying window **in the same thread**", and this host confirms the
cross-process case does not work. HTTRANSPARENT is therefore unusable for a dock that
overlaps other applications' windows.

## Question 3: does SetWindowRgn clip a DirectComposition surface and move hit-testing?

Yes, both. `spike_t010_region --mode=paint --band=200..280 --region`, run alone:

```
region=applied ret=1 band=200..280
paint=rect 500,400 600x300
sendinput=click at=800,420
sendinput=click at=800,640
event=nchittest client_y=240 result=1
event=nchittest client_y=240 result=1
event=paint_click y=240
paint=summary clicks=1
```

Two things at once:
- Pixel sampling at the band and above it (`px_region_solo.txt`): `inside (300,240)
  rgba=(255,0,255,255)` is the magenta DComp fill, `above (300,20) rgba=(12,12,12,255)`
  is the desktop behind. The region clips DComp content, which is the opposite of what
  T-005 measured for system backdrops — Mica and accent acrylic paint straight over a
  region, our own composition surface does not.
- The click at screen y=420 (client y=20, outside the band) produced **no** WM_NCHITTEST
  at all; the click at screen y=640 (client y=240, inside the band) produced
  `result=1` (HTCLIENT) and a WM_LBUTTONDOWN. Hit-testing follows the region.

## Question 4: z-order between two topmost windows

Two runs gave opposite results, both consistent with "creation order does not decide":

- First spike, overlay created second, `--click` at the window centre and at y=20:
  `overlay=summary ... clicks=2`, victim received 0. Overlay was on top.
- Second spike, paint created second with an explicit
  `SetWindowPos(HWND_TOPMOST, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE)`: victim received
  both clicks, paint received 0. Paint ended up **below**.

So an explicit SetWindowPos to HWND_TOPMOST does not guarantee being above another
topmost window; it only guarantees being above non-topmost ones. This matches the T-004
note that topmost stacking is not creation-ordered. The dock has exactly one topmost
window and no sibling, so it is unaffected — but any evidence run that stacks two
topmost windows must verify which one is on top instead of assuming.

## Consequence for the design

The dock window must grow when icons magnify (a 1.8x icon is 86px tall inside a 68px
plate). The grown area is empty space that would otherwise swallow clicks meant for the
window above the dock. Since HTTRANSPARENT does not cross processes, the only working
mechanism is SetWindowRgn: shrink the region to the plate while idle, expand it while
hovering. Region updates happen on hover enter/leave, not per frame.
