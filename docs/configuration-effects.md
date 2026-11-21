# Configuration effects

Every GUI toggle that has a visible effect, recorded as an off frame, an on frame and a looping GIF of the switch. Frames are real captures from the shipped build, taken on a real X server, so this page is also a regression check: rerunning the pipeline after a UI change reproduces them, and a toggle whose effect disappears is reported as a failure.

## Captured toggles

Each GIF is recorded as first state, switch, second state. First and second frames are also shipped as stills.

| Toggle | Where | First state | Second state |
| --- | --- | --- | --- |
| Transparent background (`透明底`) | Canvas toolbar | transparent pixels painted with the panel colour | transparent pixels painted as a checkerboard |
| View mode (`网格视图` / `列表视图`) | Main toolbar | one row per asset | grid of thumbnails |
| Zoom (`100%` / `适应窗口`) | Canvas toolbar | exact 100% | fitted to the canvas |
| Sliding compare (`滑动对比`) | Two-version compare dialog | two side-by-side panes | one pane with a draggable divider |

The compare dialog's `适配窗口（统一比例）` checkbox is deliberately outside this inventory: for square images, uniform-aspect scaling changes nothing on screen, so its off and on frames are identical and the 1% gate would report a false failure. It needs a non-square asset to have a visible effect, and faking one with a square fixture would be worse than omitting it.

## Transparent background

Off:

![Transparent background off](assets/gifs/transparent-bg-off.png)

On:

![Transparent background on](assets/gifs/transparent-bg-on.png)

![Transparent background toggle](assets/gifs/transparent-bg.gif)

## View mode

Off (one row per asset):

![View mode off](assets/gifs/view-mode-off.png)

On (grid of thumbnails):

![View mode on](assets/gifs/view-mode-on.png)

![View mode toggle](assets/gifs/view-mode.gif)

## Zoom

100%:

![Zoom off](assets/gifs/zoom-fit-off.png)

Fitted to the window:

![Zoom on](assets/gifs/zoom-fit-on.png)

![Zoom toggle](assets/gifs/zoom-fit.gif)

## Sliding compare

Side by side:

![Sliding compare off](assets/gifs/sliding-compare-off.png)

Single pane with a divider:

![Sliding compare on](assets/gifs/sliding-compare-on.png)

![Sliding compare toggle](assets/gifs/sliding-compare.gif)

## Regenerating the evidence

```sh
scripts/record-toggle-gifs.sh \
  --bin build/dev/pentimento \
  --library /path/to/a/library/with/two/versions \
  --out docs/assets/gifs
```

The script starts its own `Xvfb` when no display is reachable, pins the window to 1240x790, clicks each toggle, and measures every switch with `compare -metric AE`. A toggle whose off and on frames differ by less than 1% of the frame fails the run, which is what turns this page into a configuration-effect regression test. The captured coordinates are window-relative, which is why the window is resized and moved rather than assumed to be where a window manager put it.

Requirements: `xdotool`, `xwininfo`, `xwd`, ImageMagick (`convert`, `compare`), `Xvfb`.

The demo library only needs one asset with real transparency in its working copy and two recorded versions, so the compare dialog has history to show.
