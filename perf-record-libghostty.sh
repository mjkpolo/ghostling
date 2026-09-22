#!/bin/sh
set -eu

# Record the four libghostty-vt calls that define Ghostling's current event
# boundary. Hardware execute breakpoints give exact calls without modifying the
# program, but x86 provides only four slots, so this deliberately stays focused.

PERF=${PERF:-/usr/lib/linux-tools/6.8.0-139-generic/perf}
BINARY=${BINARY:-./build/ghostling}
PREFIX=${1:-ghostling-perf}
LIB=$(ldd "$BINARY" | awk '/libghostty-vt\.so/ { print $3; exit }')

if [ ! -x "$PERF" ]; then
    echo "perf executable not found: $PERF" >&2
    exit 1
fi
if [ ! -f "$LIB" ]; then
    echo "libghostty-vt not found through ldd" >&2
    exit 1
fi

"$BINARY" &
APP_PID=$!

cleanup() {
    if kill -0 "$APP_PID" 2>/dev/null; then
        kill "$APP_PID" 2>/dev/null || true
    fi
}
trap cleanup INT TERM HUP

# Wait until the dynamic loader has mapped libghostty-vt.
BASE_HEX=
while kill -0 "$APP_PID" 2>/dev/null; do
    BASE_HEX=$(awk '
        $3 == "00000000" && /libghostty-vt\.so/ { split($1, a, "-"); print a[1]; exit }
    ' "/proc/$APP_PID/maps")
    [ -n "$BASE_HEX" ] && break
    sleep 0.01
done

if [ -z "$BASE_HEX" ]; then
    echo "Ghostling exited before libghostty-vt was mapped" >&2
    wait "$APP_PID" || true
    exit 1
fi

symbol_offset() {
    nm -D --defined-only "$LIB" |
        awk -v symbol="$1" '$3 == symbol { print "0x" $1; exit }'
}

BASE_DEC=$((0x$BASE_HEX))
VT_ADDR=$(printf '0x%x' $((BASE_DEC + $(symbol_offset ghostty_terminal_vt_write))))
RENDER_ADDR=$(printf '0x%x' $((BASE_DEC + $(symbol_offset ghostty_render_state_update))))
KEY_ADDR=$(printf '0x%x' $((BASE_DEC + $(symbol_offset ghostty_key_encoder_setopt_from_terminal))))
MOUSE_ADDR=$(printf '0x%x' $((BASE_DEC + $(symbol_offset ghostty_mouse_encoder_setopt_from_terminal))))

echo "Recording Ghostling PID $APP_PID. Close its window to stop."
"$PERF" record -o "$PREFIX.data" -p "$APP_PID" \
    -e "mem:$VT_ADDR:x" \
    -e "mem:$RENDER_ADDR:x" \
    -e "mem:$KEY_ADDR:x" \
    -e "mem:$MOUSE_ADDR:x"

wait "$APP_PID" || true
trap - INT TERM HUP

"$PERF" script -i "$PREFIX.data" --ns \
    -F comm,pid,tid,time,event,ip,sym,dso > "$PREFIX.txt"

awk '
    NR == 1 { base = $3; sub(":$", "", base) }
    {
        time = $3
        sub(":$", "", time)
        printf "+%09.6fs  %s\n", time - base, $6
    }
' "$PREFIX.txt" > "$PREFIX-timeline.txt"

echo
echo "Call counts:"
awk '{ print $6 }' "$PREFIX.txt" | sort | uniq -c
echo
echo "Files written:"
echo "  $PREFIX.data"
echo "  $PREFIX.txt"
echo "  $PREFIX-timeline.txt"
