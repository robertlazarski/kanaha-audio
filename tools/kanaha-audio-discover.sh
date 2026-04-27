#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025-2026 Robert Lazarski
set -euo pipefail
#
# Kanaha Audio Discovery - mDNS + network scanner
# Usage: kanaha-audio-discover.sh [--json] [--ip IP] [--scan]
#
# Discovery methods (in order of preference):
#   1. avahi-browse (mDNS) - instant, requires avahi-utils
#   2. Port scanning - scans local subnet, slower but always works
#

SSL_DIR="${KANAHA_SSL_DIR:-$HOME/repos/kanaha-audio/kanaha-audio-app/app/src/main/assets/ssl}"
PORT=8443

# Parse args
JSON=false
SINGLE_IP=""
FORCE_SCAN=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --json) JSON=true; shift ;;
        --ip) SINGLE_IP="$2"; shift 2 ;;
        --scan) FORCE_SCAN=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--json] [--ip IP] [--scan]"
            echo "  --json    Output JSON"
            echo "  --ip IP   Check single IP only"
            echo "  --scan    Force port scanning (skip mDNS)"
            exit 0 ;;
        *) shift ;;
    esac
done

# Check single device via API
check_device() {
    local ip="$1"
    curl -s --connect-timeout 2 --max-time 3 \
        --cert "$SSL_DIR/client.crt" \
        --key "$SSL_DIR/client.key" \
        --cacert "$SSL_DIR/ca.crt" \
        -H "Content-Type: application/json" \
        -d '{"action":"getStatus"}' \
        "https://$ip:$PORT/services/AudioSearchService/getStatus" 2>/dev/null
}

# Discover devices via avahi-browse (mDNS)
discover_via_avahi() {
    local tmpfile="$1"

    timeout 3 avahi-browse -rp _https._tcp 2>/dev/null | while IFS=';' read -r status iface proto name type domain hostname addr port txt rest; do
        [[ "$status" != "=" ]] && continue
        [[ -z "$addr" ]] && continue

        # Check if this is a Kanaha Audio device (api=kanaha-audio in TXT)
        [[ "$txt" != *"api=kanaha-audio"* ]] && continue

        txt_clean=$(printf '%s\n' "$txt" | sed 's/" "/\n/g; s/^"//; s/"$//')

        dev_name=$(printf '%s\n' "$txt_clean" | sed -n 's/^name=//p')
        [[ -z "$dev_name" ]] && dev_name="$name"

        model=$(printf '%s\n' "$txt_clean" | sed -n 's/^model=//p')
        [[ -z "$model" ]] && model="Unknown"

        # Get status via API
        result=$(check_device "$addr")
        if echo "$result" | grep -q '"success".*true'; then
            whisper_model="$(echo "$result" | grep -oP '"current_model"\s*:\s*"\K[^"]+')"
            initialized="$(echo "$result" | grep -oP '"initialized"\s*:\s*\K[a-z]+')"
            echo "$addr|$hostname|$dev_name|$model|$whisper_model|$initialized" >> "$tmpfile"
        fi
    done
}

# Single IP mode
if [[ -n "$SINGLE_IP" ]]; then
    result=$(check_device "$SINGLE_IP")
    if echo "$result" | grep -q '"success".*true'; then
        if [[ "$JSON" = true ]]; then
            printf '[{"ip":"%s","port":%d,"status":%s}]\n' "$SINGLE_IP" "$PORT" "$result"
        else
            echo "Found Kanaha Audio device at $SINGLE_IP:$PORT"
            echo "$result" | python3 -m json.tool 2>/dev/null || echo "$result"
        fi
        exit 0
    else
        [[ "$JSON" = true ]] && echo "[]" || echo "No device at $SINGLE_IP"
        exit 1
    fi
fi

# Fast parallel scan using temp file
TMPFILE=$(mktemp)
trap "rm -f $TMPFILE" EXIT

# Port scanning function
scan_subnet() {
    local subnet="$1"
    [[ "$JSON" = false ]] && echo "Scanning $subnet.0/24..." >&2

    for i in $(seq 1 254); do
        (
            ip="$subnet.$i"
            timeout 0.5 bash -c "echo >/dev/tcp/$ip/$PORT" 2>/dev/null || exit 0
            result=$(check_device "$ip")
            if echo "$result" | grep -q '"success".*true'; then
                whisper_model="$(echo "$result" | grep -oP '"current_model"\s*:\s*"\K[^"]+')"
                initialized="$(echo "$result" | grep -oP '"initialized"\s*:\s*\K[a-z]+')"
                echo "$ip||Kanaha Audio|Unknown|$whisper_model|$initialized" >> "$TMPFILE"
            fi
        ) &
        (( i % 50 == 0 )) && wait
    done
    wait
}

# Try avahi-browse first, then fall back to port scanning
USE_AVAHI=false
if [[ "$FORCE_SCAN" = false ]] && command -v avahi-browse &>/dev/null; then
    USE_AVAHI=true
    [[ "$JSON" = false ]] && echo "Discovering Kanaha Audio devices via mDNS..." >&2
    discover_via_avahi "$TMPFILE"
fi

if [[ ! -s "$TMPFILE" ]]; then
    if [[ "$USE_AVAHI" = true && "$JSON" = false ]]; then
        echo "No devices found via mDNS, falling back to port scan..." >&2
    fi

    SUBNET=$(ip route get 8.8.8.8 2>/dev/null | grep -oP 'src \K[0-9]+\.[0-9]+\.[0-9]+')
    if [[ -z "$SUBNET" ]]; then
        [[ "$JSON" = true ]] && echo "[]" || echo "Cannot determine subnet"
        exit 1
    fi
    scan_subnet "$SUBNET"
fi

# Deduplicate
if [[ -s "$TMPFILE" ]]; then
    sort -t'|' -k1,1 -u "$TMPFILE" > "${TMPFILE}.dedup"
    mv "${TMPFILE}.dedup" "$TMPFILE"
fi

# Output
if [[ ! -s "$TMPFILE" ]]; then
    if [[ "$JSON" = true ]]; then
        echo "[]"
    else
        echo "No Kanaha Audio devices found."
        echo ""
        echo "Troubleshooting:"
        echo "  - Make sure WiFi is turned ON on the phone"
        echo "  - Verify the phone and this computer are on the same WiFi network"
        echo "  - Open the Kanaha Audio app and confirm the HTTP server is running"
        echo "  - Try: $(basename "$0") --ip <phone-ip>"
    fi
    exit 1
fi

if [[ "$JSON" = true ]]; then
    echo "["
    first=true
    while IFS='|' read -r ip hostname name model whisper_model initialized; do
        [[ "$first" = true ]] || echo ","
        first=false
        url_host="${hostname:-$ip}"
        printf '  {"name":"%s","ip":"%s","hostname":"%s","port":%d,"model":"%s","whisper_model":"%s","initialized":"%s","url":"https://%s:%d/services/AudioSearchService"}' \
            "$name" "$ip" "$hostname" "$PORT" "$model" "$whisper_model" "$initialized" "$url_host" "$PORT"
    done < "$TMPFILE"
    echo -e "\n]"
else
    count=$(wc -l < "$TMPFILE")
    echo -e "\033[32mFound $count device(s):\033[0m"
    echo ""
    while IFS='|' read -r ip hostname name model whisper_model initialized; do
        url_host="${hostname:-$ip}"
        echo "  $name ($model)"
        echo "    IP:            $ip"
        [[ -n "$hostname" ]] && echo "    Hostname:      $hostname"
        echo "    Whisper Model: ${whisper_model:-none}"
        echo "    Initialized:   $initialized"
        echo "    URL:           https://$url_host:$PORT/services/AudioSearchService"
        echo ""
    done < "$TMPFILE"
fi
