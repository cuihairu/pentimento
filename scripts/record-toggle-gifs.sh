#!/bin/bash
# DOC-GIF pipeline: record off/on evidence for every GUI toggle that has a
# visible effect, then assemble a looping GIF per toggle.
#
# Usage:
#   scripts/record-toggle-gifs.sh --bin build/dev/pentimento \
#       --library /path/to/demo-library --out docs/assets/gifs
#
# What the demo library needs: at least one asset whose working copy is a PNG
# with real transparency and that holds two or more recorded versions (so the
# two-version compare has history to show). Create it once with the GUI:
# open an empty directory as a library, import two PNGs, then replace the
# first asset's working copy and let the stability watcher record a version.
#
# Environment: an X server is required. The script starts its own Xvfb on
# $DISPLAY if none is reachable, so it also runs on a headless machine.
#
# Verification: every toggle is measured with `compare -metric AE` between its
# off and on frames. A toggle whose effect produced fewer than 1% differing
# pixels is reported and fails the run, so a UI change that silently kills a
# toggle's effect is caught here as well.

set -eu

BIN=""
LIB=""
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --bin) BIN="$2"; shift 2 ;;
    --library) LIB="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "--bin must point at an executable" >&2; exit 2; }
[ -n "$LIB" ] && [ -d "$LIB" ] || { echo "--library must be an existing library directory" >&2; exit 2; }
[ -n "$OUT" ] || { echo "--out is required" >&2; exit 2; }
for tool in xdotool xwininfo xwd convert compare xdpyinfo; do
  command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done

export DISPLAY="${DISPLAY:-:99}"
XVFB_PID=""
# Reuse a reachable server, otherwise start our own on a display number that is
# actually free. On a shared machine several sessions own :100/:101 already,
# and a bind failure aborts the app before it can create a window.
if ! xdpyinfo >/dev/null 2>&1; then
  for cand in :95 :96 :97 :98 :99 :102 :103 :104 :105; do
    if ! (DISPLAY="$cand" xdpyinfo >/dev/null 2>&1); then
      DISPLAY="$cand"
      break
    fi
  done
  export DISPLAY
  AUTH="$(mktemp -t xauth.XXXXXX)"
  xauth -f "$AUTH" generate "$DISPLAY" 2>/dev/null || true
  xauth -f "$AUTH" add "$DISPLAY" MIT-MAGIC-COOKIE-1 \
    "$(head -c 16 /dev/urandom | xxd -p)"
  export XAUTHORITY="$AUTH"
  Xvfb "$DISPLAY" -screen 0 1280x820x24 -auth "$AUTH" >/dev/null 2>&1 &
  XVFB_PID=$!
  trap 'kill "$XVFB_PID" 2>/dev/null || true' EXIT
  for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    xdpyinfo >/dev/null 2>&1 && break
    sleep 1
  done
fi
xdpyinfo >/dev/null 2>&1 || { echo "no X server reachable on $DISPLAY" >&2; exit 1; }

FRAMES="$(mktemp -d -t frames.XXXXXX)"
mkdir -p "$OUT"

# Anchored at both ends so this cannot match our own command line, which
# also contains the string "pentimento --library".
pkill -f "^${BIN} --library ${LIB}$" 2>/dev/null || true
sleep 1
APP_LOG="$(mktemp -t pentimento.XXXXXX.log)"
find_window() {
  xwininfo -root -tree 2>/dev/null | grep '画迹' | grep -o '0x[0-9a-f]*' | head -1
}
# Launch and wait for the window, once. A single retry is not worth the
# complexity; the log is kept so an abort is diagnosable instead of silent.
setsid "$BIN" --library "$LIB" >"$APP_LOG" 2>&1 < /dev/null &
APP_PID=$!
trap 'kill "$APP_PID" 2>/dev/null || true' EXIT
W=""
for i in $(seq 1 20); do
  W=$(find_window)
  [ -n "$W" ] && break
  sleep 1
done
[ -n "$W" ] || { echo "main window not found; app log:" >&2; cat "$APP_LOG" >&2; exit 1; }
sleep 2   # let the initial layout and the library load settle
xdotool windowsize "$W" 1240 790 2>/dev/null
xdotool windowmove "$W" 0 0 2>/dev/null
sleep 1

N=0
shot() {
  N=$((N + 1))
  local id
  id=$(printf "f%03d" "$N")
  xwd -id "$1" -out "$FRAMES/$id.xwd" 2>/dev/null
  convert "$FRAMES/$id.xwd" "$FRAMES/$id.png" 2>/dev/null
  echo "$N"
}
click() {
  xdotool mousemove --sync "$1" "$2" 2>/dev/null
  sleep 0.3
  xdotool click 1 2>/dev/null
  sleep 1.6
}
# One toggle: capture the off state, click, capture the on state.
# Emits "<offFrame> <onFrame> <diffPixels>" on stdout.
toggle() {
  local window="$2" x="$3" y="$4" a d diff
  a=$(shot "$window"); shot "$window" >/dev/null; shot "$window" >/dev/null
  click "$x" "$y"
  d=$(shot "$window"); shot "$window" >/dev/null; shot "$window" >/dev/null
  diff=$(compare -metric AE -fuzz 2% \
    "$FRAMES/$(printf 'f%03d' "$a").png" \
    "$FRAMES/$(printf 'f%03d' "$d").png" null: 2>&1 | grep -oE '^[0-9]+' || true)
  diff=${diff:-0}
  echo "$a $d $diff"
}
# Assemble one looping GIF and its two stills from off/on frame numbers.
assemble() {
  local name="$1" off="$2" on="$3"
  local o n
  o=$(printf 'f%03d' "$off"); n=$(printf 'f%03d' "$on")
  convert -delay 250 -loop 0 \
    "$FRAMES/$o.png" "$FRAMES/$o.png" "$FRAMES/$o.png" \
    "$FRAMES/$n.png" "$FRAMES/$n.png" "$FRAMES/$n.png" \
    -resize 1000x -layers Optimize "$OUT/$name.gif"
  convert "$FRAMES/$o.png" -resize 1000x -strip "$OUT/$name-off.png"
  convert "$FRAMES/$n.png" -resize 1000x -strip "$OUT/$name-on.png"
}

MIN_PX=12400   # 1% of a 1240x790 frame
NAMES=()
OFF_F=()
ON_F=()
DIFF_PX=()

# Record one toggle, assemble its GIF and stills, and remember the measured
# effect size for the verification report.
record() {
  local name="$1" window="$2" x="$3" y="$4" a d diff
  read -r a d diff <<<"$(toggle "$name" "$window" "$x" "$y")"
  assemble "$name" "$a" "$d"
  NAMES+=("$name"); OFF_F+=("$a"); ON_F+=("$d"); DIFF_PX+=("${diff%.*}")
}

# --- main window toggles (window coordinates, window pinned to 1240x790) ----
# 透明底: render transparent regions as a checkerboard instead of the panel colour.
record transparent-bg "$W" 535 93
# 网格视图 / 列表视图: grid thumbnails vs. one row per asset.
record view-mode "$W" 512 18
# 100% / 适应窗口: exact zoom vs. fitted zoom.
record zoom-fit "$W" 373 93
# 对比版本… opens 双版对比; inside it, 滑动对比 switches side-by-side to a
# single pane with a draggable divider. The dialog opens centred at +180+75.
shot "$W" >/dev/null
click 1096 377
sleep 2.5
D=$(xwininfo -root -tree 2>/dev/null | grep '双版对比' | grep -o '0x[0-9a-f]*' | head -1)
[ -n "$D" ] || { echo "compare dialog not found" >&2; exit 1; }
record sliding-compare "$D" 668 560
# 关闭 the dialog so a later run starts from the main view.
xdotool mousemove --sync 610 610 2>/dev/null
sleep 0.3
xdotool key --window "$D" Escape 2>/dev/null || true

echo "---"
fail=0
for i in "${!NAMES[@]}"; do
  printf '%-18s off=f%-3s on=f%-3s diff=%s px\n' \
    "${NAMES[$i]}" "${OFF_F[$i]}" "${ON_F[$i]}" "${DIFF_PX[$i]}"
  if [ "${DIFF_PX[$i]}" -lt "$MIN_PX" ]; then
    echo "FAIL: ${NAMES[$i]} produced less than 1% differing pixels" >&2
    fail=1
  fi
done
rm -rf "$FRAMES" "$APP_LOG"
echo "wrote $OUT"
exit $fail
