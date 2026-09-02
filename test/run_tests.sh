#!/bin/bash
set -euo pipefail

PORT="${ESPPORT:-/dev/cu.usbmodem2101}"
IDF_PATH="${TEST_IDF_PATH:-/Users/lamphuchai/.espressif/v6.1-rc1/esp-idf}"
IDF_PYTHON_ENV_PATH="${TEST_IDF_PYTHON_ENV_PATH:-/Users/lamphuchai/.espressif/python_env/idf6.1_py3.13_env}"
SERIAL_BAUD=115200
READ_TIMEOUT=180

[[ -f "$IDF_PATH/export.sh" ]] || {
    echo "ESP-IDF export.sh not found: $IDF_PATH/export.sh" >&2
    exit 1
}
export IDF_PATH IDF_PYTHON_ENV_PATH
# ESP-IDF 6.1 selects its matching Python environment and toolchain here.
# Do not hard-code compiler/bin paths: those changed from IDF 5.4 to 6.1.
source "$IDF_PATH/export.sh" >/dev/null
IDF_PYTHON="$(command -v python)"

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$TEST_DIR/build"
RESULT_FILE="$TEST_DIR/test_results.txt"
SERIAL_LOG="$TEST_DIR/test_serial.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

usage() {
    echo "Usage: $0 [OPTIONS] [PORT]"
    echo ""
    echo "Options:"
    echo "  -b, --build-only    Build only, skip flash/monitor"
    echo "  -f, --flash-only    Flash only, skip build"
    echo "  -m, --monitor-only  Monitor only, skip build/flash"
    echo "  -s, --skip-build    Flash and monitor without rebuilding"
    echo "  -c, --clean         Full-clean before building"
    echo "  -t, --timeout SEC   Serial read timeout (default: $READ_TIMEOUT)"
    echo "  -h, --help          Show this help"
    echo ""
    echo "Examples:"
    echo "  $0                           # Full build + flash + monitor"
    echo "  $0 /dev/tty.usbmodem2101     # Specify port"
    echo "  $0 -b                        # Build only"
    echo "  $0 -m                        # Monitor only (device already flashed)"
}

BUILD=1
FLASH=1
MONITOR=1
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-only)   FLASH=0; MONITOR=0; shift ;;
        -f|--flash-only)   BUILD=0; MONITOR=0; shift ;;
        -m|--monitor-only) BUILD=0; FLASH=0; shift ;;
        -s|--skip-build)   BUILD=0; shift ;;
        -c|--clean)        CLEAN=1; shift ;;
        -t|--timeout)      READ_TIMEOUT="$2"; shift 2 ;;
        -h|--help)         usage; exit 0 ;;
        -*)                echo "Unknown option: $1"; usage; exit 1 ;;
        *)                 PORT="$1"; shift ;;
    esac
done

log() {
    echo -e "${CYAN}[$(date +%H:%M:%S)]${NC} $1"
}

step() {
    echo -e "\n${BOLD}${GREEN}═══ $1 ═══${NC}"
}

fail() {
    echo -e "${RED}ERROR: $1${NC}" >&2
    exit 1
}

if [[ ! -e "$PORT" ]]; then
    usb_ports=(/dev/cu.usbmodem*)
    if [[ ${#usb_ports[@]} -eq 1 && -e "${usb_ports[0]}" ]]; then
        PORT="${usb_ports[0]}"
    elif [[ $FLASH -eq 1 || $MONITOR -eq 1 ]]; then
        fail "Serial port not found: $PORT"
    fi
fi

log "Using $(idf.py --version)"
log "Python: $IDF_PYTHON"

# ── Build ──────────────────────────────────────────────────────────────
if [[ $BUILD -eq 1 ]]; then
    step "Building test project"
    cd "$TEST_DIR"
    if [[ -f "$BUILD_DIR/CMakeCache.txt" ]] &&
       ! grep -Fq "$IDF_PATH" "$BUILD_DIR/CMakeCache.txt"; then
        log "Build cache belongs to another ESP-IDF; running fullclean"
        idf.py fullclean
    elif [[ $CLEAN -eq 1 ]]; then
        idf.py fullclean
    fi
    if [[ ! -f "$TEST_DIR/sdkconfig" ]]; then
        idf.py set-target esp32s3
    fi
    idf.py build
    log "Build complete"
fi

# ── Flash ──────────────────────────────────────────────────────────────
if [[ $FLASH -eq 1 ]]; then
    step "Flashing to $PORT"
    cd "$TEST_DIR"
    idf.py -p "$PORT" flash
    log "Flash complete"
fi

# ── Monitor ────────────────────────────────────────────────────────────
if [[ $MONITOR -eq 1 ]]; then
    step "Running tests on device"

    "$IDF_PYTHON" - "$PORT" "$SERIAL_BAUD" "$READ_TIMEOUT" "$SERIAL_LOG" <<'PY'
import re
import serial
import sys
import time

port, baud, timeout, log_path = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
ser = serial.Serial(port, baud, timeout=1)
time.sleep(0.3)
ser.reset_input_buffer()

# Hardware reset
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False

start = time.monotonic()
output = []
summary_seen_at = None
last_output_at = time.monotonic()
test_count = 0
try:
    while time.monotonic() - start < timeout:
        raw = ser.readline()
        if raw:
            line = raw.decode('utf-8', errors='replace').rstrip()
            output.append(line)
            last_output_at = time.monotonic()
            # Live print key lines
            if ('PASS' in line or 'FAIL' in line or 'Running' in line or
                    'assert failed' in line or 'Guru Meditation' in line):
                print(line, flush=True)
                if ':PASS' in line or ':FAIL' in line:
                    test_count += 1
            elif 'Test ' in line and 'summary' in line.lower():
                print(line, flush=True)
            if re.search(r'\b\d+ Tests? \d+ Failures? \d+ Ignored\b', line):
                summary_seen_at = time.monotonic()
        else:
            # No data for 3s — device might be at Unity menu, send Enter
            if time.monotonic() - last_output_at > 3.0:
                ser.write(b'\n')
                ser.flush()
                last_output_at = time.monotonic()
                print("[auto] Sent Enter (idle 3s)", flush=True)
        if summary_seen_at is not None and time.monotonic() - summary_seen_at >= 2.0:
            break
except KeyboardInterrupt:
    pass
finally:
    ser.close()

with open(log_path, 'w', encoding='utf-8') as f:
    for l in output:
        f.write(l + '\n')
PY

    log "Serial log saved to $SERIAL_LOG"

    # ── Parse results ──────────────────────────────────────────────────
    step "Test Results"

    "$IDF_PYTHON" - "$SERIAL_LOG" <<'PY' | tee "$RESULT_FILE"
import re, sys
from collections import defaultdict

with open(sys.argv[1], encoding='utf-8') as f:
    lines = [l.strip() for l in f.readlines()]

passes = []
fails = []
comp = defaultdict(lambda: {'pass': 0, 'fail': 0, 'names': []})

for line in lines:
    if ':PASS' in line:
        passes.append(line)
        m = re.search(r'components/(\w+)/test/', line)
        c = m.group(1) if m else 'other'
        comp[c]['pass'] += 1
        # extract test name
        m2 = re.search(r':(.+):PASS', line)
        if m2:
            comp[c]['names'].append(('PASS', m2.group(1)))
    elif ':FAIL' in line:
        fails.append(line)
        m = re.search(r'components/(\w+)/test/', line)
        c = m.group(1) if m else 'other'
        comp[c]['fail'] += 1
        m2 = re.search(r':(.+):FAIL', line)
        if m2:
            comp[c]['names'].append(('FAIL', m2.group(1)))

total = len(passes) + len(fails)
summary = next((line for line in reversed(lines)
                if re.search(r'\b\d+ Tests? \d+ Failures? \d+ Ignored\b', line)), None)
fatal_lines = [line for line in lines
               if 'assert failed:' in line or 'Guru Meditation Error' in line]

if total == 0:
    print('No tests detected. Device may not have booted into test mode.')
    sys.exit(1)

# Summary table
print()
print(f"  {'Component':<25} {'PASS':>5} {'FAIL':>5} {'Total':>6}  Status")
print('  ' + '─' * 55)
for c in sorted(comp.keys()):
    r = comp[c]
    t = r['pass'] + r['fail']
    status = ('\033[32mALL PASS\033[0m' if r['fail'] == 0
              else f"\033[31m{r['fail']} FAILED\033[0m")
    print(f"  {c:<25} {r['pass']:>5} {r['fail']:>5} {t:>6}  {status}")
print('  ' + '─' * 55)

color = '\033[32m' if fails == 0 else '\033[31m'
print(f"  {color}{'TOTAL':<25} {len(passes):>5} {len(fails):>5} {total:>6}\033[0m")
print()

if summary:
    print(f'  Unity summary: {summary}')
else:
    print('  \033[31mUnity summary missing: test suite did not finish.\033[0m')

if fatal_lines:
    print('  \033[31mFatal runtime errors:\033[0m')
    for line in fatal_lines:
        print(f'    {line}')
    print()

if fails:
    print('  \033[31mFailed tests:\033[0m')
    for f in fails:
        m = re.search(r'test_\w+\.c:(\d+):(.+):FAIL(.*)', f)
        if m:
            print(f'    line {m.group(1)}: {m.group(2)}{m.group(3)}')
        else:
            print(f'    {f}')
    print()

sys.exit(0 if not fails and summary and not fatal_lines else 1)
PY
fi
