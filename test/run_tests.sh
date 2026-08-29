#!/bin/bash
set -euo pipefail

PORT="${1:-/dev/cu.usbmodem2101}"
IDF_PATH="${IDF_PATH:-/Users/lamphuchai/.espressif/v6.1-rc1/esp-idf}"
IDF_PYTHON="${IDF_PYTHON:-/Users/lamphuchai/.espressif/python_env/idf6.1_py3.13_env/bin/python}"
SERIAL_BAUD=115200
READ_TIMEOUT=50

export IDF_PATH
export PATH="/Users/lamphuchai/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:/Users/lamphuchai/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:/Users/lamphuchai/.espressif/tools/esp32ulp-elf/esp-14.2.0_20260121/esp32ulp-elf/bin:/Users/lamphuchai/.espressif/tools/ninja/1.12.1:$PATH"
export PYTHON="$IDF_PYTHON"

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

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-only)   FLASH=0; MONITOR=0; shift ;;
        -f|--flash-only)   BUILD=0; MONITOR=0; shift ;;
        -m|--monitor-only) BUILD=0; FLASH=0; shift ;;
        -s|--skip-build)   BUILD=0; shift ;;
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

# ── Build ──────────────────────────────────────────────────────────────
if [[ $BUILD -eq 1 ]]; then
    step "Building test project"
    cd "$TEST_DIR"
    rm -rf "$BUILD_DIR"
    $PYTHON "$IDF_PATH/tools/idf.py" set-target esp32s3 2>&1 | tail -3
    $PYTHON "$IDF_PATH/tools/idf.py" build 2>&1 | tail -5
    log "Build complete"
fi

# ── Flash ──────────────────────────────────────────────────────────────
if [[ $FLASH -eq 1 ]]; then
    step "Flashing to $PORT"
    cd "$TEST_DIR"
    $PYTHON "$IDF_PATH/tools/idf.py" -p "$PORT" flash 2>&1 | tail -3
    log "Flash complete"
fi

# ── Monitor ────────────────────────────────────────────────────────────
if [[ $MONITOR -eq 1 ]]; then
    step "Running tests on device"

    $IDF_PYTHON -c "
import serial, time, sys

ser = serial.Serial('$PORT', $SERIAL_BAUD, timeout=2)
time.sleep(0.3)

# Hardware reset
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False

start = time.time()
output = []
try:
    while time.time() - start < $READ_TIMEOUT:
        raw = ser.readline()
        if raw:
            line = raw.decode('utf-8', errors='replace').rstrip()
            output.append(line)
            # Live print key lines
            if 'PASS' in line or 'FAIL' in line or 'Running' in line:
                print(line, flush=True)
            elif 'Test ' in line and 'summary' in line.lower():
                print(line, flush=True)
except KeyboardInterrupt:
    pass
finally:
    ser.close()

with open('$SERIAL_LOG', 'w') as f:
    for l in output:
        f.write(l + '\n')
"

    log "Serial log saved to $SERIAL_LOG"

    # ── Parse results ──────────────────────────────────────────────────
    step "Test Results"

    $IDF_PYTHON -c "
import re, sys
from collections import defaultdict

with open('$SERIAL_LOG') as f:
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

if total == 0:
    print('No tests detected. Device may not have booted into test mode.')
    sys.exit(1)

# Summary table
print()
print(f'  {\"Component\":<25} {\"PASS\":>5} {\"FAIL\":>5} {\"Total\":>6}  Status')
print('  ' + '─' * 55)
for c in sorted(comp.keys()):
    r = comp[c]
    t = r['pass'] + r['fail']
    status = f'\033[32mALL PASS\033[0m' if r['fail'] == 0 else f'\033[31m{r[\"fail\"]} FAILED\033[0m'
    print(f'  {c:<25} {r[\"pass\"]:>5} {r[\"fail\"]:>5} {t:>6}  {status}')
print('  ' + '─' * 55)

color = '\033[32m' if fails == 0 else '\033[31m'
print(f'  {color}{\"TOTAL\":<25} {len(passes):>5} {len(fails):>5} {total:>6}\033[0m')
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

sys.exit(0 if fails == 0 else 1)
"
fi
