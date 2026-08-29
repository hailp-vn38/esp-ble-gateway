#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Có thể override các đường dẫn này bằng biến môi trường tương ứng.
IDF_INSTALL_DIR="${GATEWAY_IDF_PATH:-/Users/lamphuchai/.espressif/v6.1-rc1/esp-idf}"
PYTHON_ENV_DIR="${GATEWAY_IDF_PYTHON_ENV_PATH:-/Users/lamphuchai/.espressif/python_env/idf6.1_py3.13_env}"
SERIAL_PORT="${ESPPORT:-}"
FLASH_BAUD="${ESPBAUD:-460800}"

DO_BUILD=1
DO_FLASH=1
DO_MONITOR=0
DO_CLEAN=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [PORT]

Kích hoạt ESP-IDF, build và flash firmware ESP32-S3.

Options:
  -p, --port PORT     Cổng serial, ví dụ /dev/cu.usbmodem2101
  -B, --baud BAUD     Baud khi flash (mặc định: $FLASH_BAUD)
  -b, --build-only    Chỉ build, không flash
  -f, --flash-only    Chỉ flash firmware đã build
  -m, --monitor       Mở serial monitor sau khi flash
  -c, --clean         Chạy idf.py fullclean trước khi build
  -h, --help          Hiển thị trợ giúp

Examples:
  ./build_flash.sh
  ./build_flash.sh /dev/cu.usbmodem2101
  ./build_flash.sh --port /dev/ttyACM0 --monitor
  ./build_flash.sh --build-only

Environment overrides:
  GATEWAY_IDF_PATH
  GATEWAY_IDF_PYTHON_ENV_PATH
  ESPPORT
  ESPBAUD
EOF
}

log() {
    printf '\n[%s] %s\n' "$(date +%H:%M:%S)" "$1"
}

fail() {
    printf 'ERROR: %s\n' "$1" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)
            [[ $# -ge 2 ]] || fail "$1 yêu cầu giá trị PORT"
            SERIAL_PORT="$2"
            shift 2
            ;;
        -B|--baud)
            [[ $# -ge 2 ]] || fail "$1 yêu cầu giá trị BAUD"
            FLASH_BAUD="$2"
            shift 2
            ;;
        -b|--build-only)
            DO_FLASH=0
            DO_MONITOR=0
            shift
            ;;
        -f|--flash-only)
            DO_BUILD=0
            shift
            ;;
        -m|--monitor)
            DO_MONITOR=1
            shift
            ;;
        -c|--clean)
            DO_CLEAN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            fail "Tùy chọn không hợp lệ: $1"
            ;;
        *)
            [[ -z "$SERIAL_PORT" ]] || fail "Chỉ được chỉ định một cổng serial"
            SERIAL_PORT="$1"
            shift
            ;;
    esac
done

[[ "$FLASH_BAUD" =~ ^[0-9]+$ ]] || fail "Baud không hợp lệ: $FLASH_BAUD"
[[ -f "$IDF_INSTALL_DIR/export.sh" ]] || fail "Không tìm thấy $IDF_INSTALL_DIR/export.sh"
[[ -d "$PYTHON_ENV_DIR" ]] || fail "Không tìm thấy Python environment: $PYTHON_ENV_DIR"

export IDF_PATH="$IDF_INSTALL_DIR"
export IDF_PYTHON_ENV_PATH="$PYTHON_ENV_DIR"

log "Kích hoạt ESP-IDF environment"
# export.sh thiết lập idf.py, Python environment và toolchain tương ứng.
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null

command -v idf.py >/dev/null 2>&1 || fail "idf.py không có trên PATH sau khi kích hoạt environment"
log "Sử dụng $(idf.py --version)"
log "Python: $(command -v python)"

cd "$PROJECT_DIR"

if [[ $DO_BUILD -eq 1 ]]; then
    if [[ ! -f components/qcbor_lib/QCBOR/inc/qcbor/qcbor_encode.h ]]; then
        log "Khởi tạo Git submodule QCBOR"
        git submodule update --init --recursive
    fi

    if [[ $DO_CLEAN -eq 1 ]]; then
        log "Dọn build directory"
        idf.py fullclean
    elif [[ -f build/CMakeCache.txt ]] &&
         ! grep -Fq "$IDF_PATH" build/CMakeCache.txt; then
        log "Build cache thuộc ESP-IDF khác; chạy fullclean"
        idf.py fullclean
    fi

    if [[ ! -f sdkconfig ]]; then
        log "Thiết lập target esp32s3"
        idf.py set-target esp32s3
    elif ! grep -Fqx 'CONFIG_IDF_TARGET="esp32s3"' sdkconfig; then
        fail "sdkconfig hiện không dùng target esp32s3; hãy kiểm tra config trước khi chạy lại"
    fi

    log "Build firmware"
    idf.py build
fi

if [[ $DO_FLASH -eq 1 || $DO_MONITOR -eq 1 ]]; then
    if [[ -n "$SERIAL_PORT" ]]; then
        [[ -e "$SERIAL_PORT" ]] || fail "Không tìm thấy cổng serial: $SERIAL_PORT"
    else
        shopt -s nullglob
        serial_candidates=(
            /dev/cu.usbmodem*
            /dev/cu.usbserial*
            /dev/ttyACM*
            /dev/ttyUSB*
        )
        shopt -u nullglob

        if [[ ${#serial_candidates[@]} -eq 1 ]]; then
            SERIAL_PORT="${serial_candidates[0]}"
            log "Tự phát hiện cổng serial: $SERIAL_PORT"
        elif [[ ${#serial_candidates[@]} -gt 1 ]]; then
            printf 'Phát hiện nhiều cổng serial:\n' >&2
            printf '  %s\n' "${serial_candidates[@]}" >&2
            fail "Hãy chọn cổng bằng --port PORT"
        else
            fail "Không tìm thấy cổng serial; hãy kết nối board hoặc dùng --port PORT"
        fi
    fi
fi

if [[ $DO_FLASH -eq 1 ]]; then
    log "Flash firmware qua $SERIAL_PORT ở baud $FLASH_BAUD"
    idf.py -p "$SERIAL_PORT" -b "$FLASH_BAUD" flash
    log "Flash hoàn tất"
fi

if [[ $DO_MONITOR -eq 1 ]]; then
    log "Mở serial monitor; nhấn Ctrl+] để thoát"
    idf.py -p "$SERIAL_PORT" monitor
fi

