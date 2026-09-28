#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025-2026 Robert Lazarski
#
# Kanaha Discovery - find every Kanaha app on the local network: Kanaha Camera,
# Kanaha Audio and Kanaha Calcs.
#
# The same file lives in all three projects, under the same name --
# tools/kanaha-discover.sh in kanaha, kanaha-audio and kanaha-calcs -- so it
# answers the same wherever it is run from. Change all three together.
#
# Usage: kanaha-discover.sh [--kind camera|audio|calcs] [--json] [--ip IP] [--scan]
#
# How it finds them, in order:
#   1. mDNS. Every Kanaha app advertises _https._tcp while its server runs, with
#      its kind in the TXT record: api=kanaha-camera-control, api=kanaha-audio-search
#      or api=kanaha-calcs. Instant, needs avahi-browse (avahi-utils).
#   2. A port scan of this machine's /24, used only when mDNS finds nothing.
#      Slower, and it cannot tell apps apart until it asks each one.
#
# Every app found is then asked for its status over mTLS with full certificate
# verification. A certificate that names its host by DNS name rather than IP
# (camera.local, calcs.local) is verified against that name with curl --resolve,
# the equivalent of the Axis2/C client's verify_name. An app that answers mDNS
# but not the status call is still listed, marked unreachable, with the reason.
#
# Certificates: $KANAHA_CERT_DIR (default ~/kanaha-ca) holding operator.crt,
# operator.key and kanaha-ca.crt, or client.crt, client.key and ca.crt.
# The names verified: $KANAHA_CAMERA_TLS_NAME (camera.local),
# $KANAHA_CALCS_TLS_NAME (calcs.local), $KANAHA_AUDIO_TLS_NAME (audio.local).
# Kanaha Audio's certificate also carries the IP it had when provisioned, so it
# is tried by IP first; on another network the name is what verifies.
#
# Scripts that act on one kind should pass --kind: a phone can run several
# apps, so without it one IP can appear more than once.

# No -e, on purpose: timeout exits 124 when avahi-browse finds nothing, and
# with -e and pipefail the script would die there instead of port scanning.
set -uo pipefail

CERT_DIR="${KANAHA_CERT_DIR:-${KANAHA_SSL_DIR:-$HOME/kanaha-ca}}"
if [[ -f "$CERT_DIR/operator.crt" ]]; then
    CERT="$CERT_DIR/operator.crt"; KEY="$CERT_DIR/operator.key"; CA="$CERT_DIR/kanaha-ca.crt"
else
    CERT="$CERT_DIR/client.crt"; KEY="$CERT_DIR/client.key"; CA="$CERT_DIR/ca.crt"
fi
for f in "$CERT" "$KEY" "$CA"; do
    [[ -r "$f" ]] || echo "warning: $f is missing; apps will be found but not reachable (set KANAHA_CERT_DIR)" >&2
done

JSON=false
SINGLE_IP=""
FORCE_SCAN=false
ONLY=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --json) JSON=true; shift ;;
        --ip) SINGLE_IP="${2:-}"; shift 2 ;;
        --scan) FORCE_SCAN=true; shift ;;
        --kind) ONLY="${2:-}"; shift 2 ;;
        -h|--help)
            echo "Usage: $(basename "$0") [--kind camera|audio|calcs] [--json] [--ip IP] [--scan]"
            echo "  --kind K  Only this kind of Kanaha app"
            echo "  --json    Output JSON (an array; each entry has a \"kind\")"
            echo "  --ip IP   Check one address only, for every kind"
            echo "  --scan    Force the port scan (skip mDNS)"
            exit 0 ;;
        *) shift ;;
    esac
done
case "$ONLY" in ""|camera|audio|calcs) ;; *) echo "--kind must be camera, audio or calcs" >&2; exit 2 ;; esac

say() { [[ "$JSON" = false ]] && echo "$@" >&2; }

# ---- the kinds -------------------------------------------------------------
# kind -> default port, service, status operation, TLS name.

kind_of()    { case $1 in kanaha-camera-control) echo camera ;; kanaha-audio-search) echo audio ;; kanaha-calcs) echo calcs ;; esac; }
port_of()    { case $1 in calcs) echo 8444 ;; *) echo 8443 ;; esac; }
service_of() { case $1 in camera) echo CameraControlService ;; audio) echo AudioSearchService ;; calcs) echo FinancialBenchmarkService ;; esac; }
op_of()      { case $1 in calcs) echo listCsvFiles ;; *) echo getStatus ;; esac; }
tls_name_of() {
    case $1 in
        camera) echo "${KANAHA_CAMERA_TLS_NAME:-camera.local}" ;;
        calcs)  echo "${KANAHA_CALCS_TLS_NAME:-calcs.local}" ;;
        audio)  echo "${KANAHA_AUDIO_TLS_NAME:-audio.local}" ;;
    esac
}
wanted() { [[ -z "$ONLY" || "$ONLY" = "$1" ]]; }

# ---- asking one app for its status ------------------------------------------

# post KIND IP PORT OP: the response body on stdout; on failure, curl's reason
# on stderr and a non-zero status. By IP first; if the certificate names a DNS
# name instead, again with --resolve to that name. Never -k.
post() {
    local kind=$1 ip=$2 port=$3 op=$4 name out rc
    local args=(-s --http2 --connect-timeout 2 --max-time 5 --cert "$CERT" --key "$KEY" --cacert "$CA"
                -H "Content-Type: application/json" -d '{}' -w '\n%{errormsg}')
    local path="/services/$(service_of "$kind")/$op"
    out=$(curl "${args[@]}" "https://$ip:$port$path" 2>/dev/null); rc=$?
    name=$(tls_name_of "$kind")
    if [[ $rc -ne 0 && -n "$name" ]]; then
        out=$(curl "${args[@]}" --resolve "$name:$port:$ip" "https://$name:$port$path" 2>/dev/null); rc=$?
    fi
    if [[ $rc -ne 0 ]]; then
        out=${out##*$'\n'}
        echo "${out:-curl exit $rc}" >&2
        return $rc
    fi
    printf '%s\n' "${out%$'\n'*}"
}

# summarise KIND: reads a status body, prints "<extra-json>US<one-line summary>"
# or nothing when the body is not a successful status for that kind.
summarise() {
    python3 -c '
import json, sys
kind = sys.argv[1]
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(1)
if kind == "camera":
    if d.get("success") is not True: sys.exit(1)
    s = d.get("status", d)
    x = {"device_name": s.get("device_name"), "state": s.get("state"),
         "battery": s.get("battery_level"), "storage_mb": s.get("storage_available_mb")}
    line = "%s, battery %s%%, %s MB free" % (x["state"], x["battery"], x["storage_mb"])
elif kind == "audio":
    if d.get("success") is not True: sys.exit(1)
    w = d.get("whisper", {})
    x = {"whisper_model": w.get("current_model"), "initialized": w.get("initialized"),
         "recording": d.get("recording_active")}
    line = "whisper %s%s%s" % (x["whisper_model"] or "none",
                               "" if x["initialized"] else " (not initialised)",
                               ", recording " + str(d.get("recording_clip")) if x["recording"] else "")
elif kind == "calcs":
    if d.get("status") != "SUCCESS": sys.exit(1)
    x = {"n_files": d.get("n_files"), "files": [f.get("file") for f in d.get("files", [])]}
    line = "%s CSV file(s)" % x["n_files"]
print(json.dumps(x) + "\x1f" + line)
' "$1"
}

# probe KIND IP PORT NAME MODEL MANUFACTURER HOSTNAME: one record into $TMPFILE.
probe() {
    local kind=$1 ip=$2 port=$3 name=$4 model=$5 manu=$6 host=$7 body err sum
    local errf; errf=$(mktemp)
    # Twice: a phone whose WiFi is dozing can miss the first 2 s connect.
    if { body=$(post "$kind" "$ip" "$port" "$(op_of "$kind")" 2>"$errf") ||
         body=$(post "$kind" "$ip" "$port" "$(op_of "$kind")" 2>"$errf"); } &&
       sum=$(printf '%s' "$body" | summarise "$kind"); then
        printf "%s${US}%s${US}%s${US}%s${US}%s${US}%s${US}%s${US}true${US}${US}%s\n" "$kind" "$ip" "$port" "$name" "$model" "$manu" "$host" "$sum" >> "$TMPFILE"
    else
        err=$(tr -d '\n\037' < "$errf"); [[ -z "$err" ]] && err="no valid $(op_of "$kind") response"
        printf "%s${US}%s${US}%s${US}%s${US}%s${US}%s${US}%s${US}false${US}%s${US}{}${US}\n" "$kind" "$ip" "$port" "$name" "$model" "$manu" "$host" "$err" >> "$TMPFILE"
    fi
    rm -f "$errf"
}

# Records in $TMPFILE, one per app, fields split by the ASCII unit separator:
# kind ip port name model manufacturer hostname reachable error extra-json summary
# (a tab would not do: bash's read merges runs of tabs, losing empty fields).
US=$'\x1f'
TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE" "$TMPFILE.sorted"' EXIT

# ---- 1. mDNS -------------------------------------------------------------------

discover_via_avahi() {
    # -r resolve, -p parsable, -t stop once the cache is dumped. Fields:
    # =;iface;proto;name;type;domain;hostname;addr;port;"k=v" "k=v" ...
    # avahi reports each service once per protocol; duplicates go at the end.
    local status iface proto name type domain host addr port txt kind api model manu
    while IFS=';' read -r status iface proto name type domain host addr port txt; do
        [[ "$status" = "=" && -n "$addr" ]] || continue
        api=$(printf '%s\n' "$txt" | grep -oP '"api=\K[^"]+')
        kind=$(kind_of "$api")
        [[ -n "$kind" ]] && wanted "$kind" || continue
        # The address mDNS resolved is current. The "ip" TXT record is written
        # when the app starts and goes stale when the phone changes network
        # (seen 2026-09-26 on moving to a hotspot), so use it only in place of
        # an IPv6 address.
        if [[ "$addr" == *:* ]]; then
            local txt_ip; txt_ip=$(printf '%s\n' "$txt" | grep -oP '"ip=\K[^"]+')
            [[ -n "$txt_ip" ]] && addr=$txt_ip
        fi
        model=$(printf '%s\n' "$txt" | grep -oP '"model=\K[^"]+'); manu=$(printf '%s\n' "$txt" | grep -oP '"manufacturer=\K[^"]+')
        probe "$kind" "$addr" "$port" "$name" "${model:-Unknown}" "${manu:-Unknown}" "$host"
    done < <(timeout 8 avahi-browse -rpt _https._tcp 2>/dev/null | sort -u -t';' -k4,4 -k8,9 || true)
}

# ---- 2. port scan -------------------------------------------------------------

# check_ip IP: every wanted kind on its default port, if the port is open.
check_ip() {
    local ip=$1 kind port
    for kind in camera audio calcs; do
        wanted "$kind" || continue
        port=$(port_of "$kind")
        timeout 0.5 bash -c "echo >/dev/tcp/$ip/$port" 2>/dev/null || continue
        probe "$kind" "$ip" "$port" "" "Unknown" "Unknown" ""
    done
}

scan_subnet() {
    local subnet=$1 i
    say "Scanning $subnet.0/24..."
    for i in $(seq 1 254); do
        check_ip "$subnet.$i" &
        (( i % 50 == 0 )) && wait
    done
    wait
}

# In a scan, an open port that is not this kind answers with an error, not a
# status; only the kinds that answered are kept.
keep_answered_only() {
    [[ -s "$TMPFILE" ]] || return
    awk -F"$US" '$8 == "true"' "$TMPFILE" > "$TMPFILE.sorted"; mv "$TMPFILE.sorted" "$TMPFILE"
}

# ---- run ------------------------------------------------------------------

if [[ -n "$SINGLE_IP" ]]; then
    check_ip "$SINGLE_IP"
    keep_answered_only
else
    if [[ "$FORCE_SCAN" = false ]] && command -v avahi-browse &>/dev/null; then
        say "Discovering Kanaha apps via mDNS..."
        discover_via_avahi
        # On a phone's hotspot the default gateway is that phone, and Android
        # answers mDNS on its tethering interface only intermittently (seen
        # 2026-09-26), so any kind mDNS did not return is tried there.
        GW=$(ip route 2>/dev/null | awk '/^default/ {print $3; exit}')
        if [[ -n "$GW" ]]; then
            for kind in camera audio calcs; do
                wanted "$kind" || continue
                grep -q "^$kind$US" "$TMPFILE" 2>/dev/null && continue
                port=$(port_of "$kind")
                timeout 1 bash -c "echo >/dev/tcp/$GW/$port" 2>/dev/null || continue
                probe "$kind" "$GW" "$port" "" "Unknown" "Unknown" ""
            done
            keep_answered_only
        fi
    fi
    if [[ ! -s "$TMPFILE" ]]; then
        [[ "$FORCE_SCAN" = false ]] && say "Nothing found via mDNS, falling back to a port scan..."
        SUBNET=$(ip route get 8.8.8.8 2>/dev/null | grep -oP 'src \K[0-9]+\.[0-9]+\.[0-9]+')
        if [[ -z "$SUBNET" ]]; then
            [[ "$JSON" = true ]] && echo "[]" || echo "Cannot determine the local subnet"
            exit 1
        fi
        scan_subnet "$SUBNET"
        keep_answered_only
    fi
fi

# One line per kind, address and port, in kind order.
if [[ -s "$TMPFILE" ]]; then
    sort -t"$US" -k1,1 -k2,2V -k3,3 -u "$TMPFILE" > "$TMPFILE.sorted"
    mv "$TMPFILE.sorted" "$TMPFILE"
fi

if [[ ! -s "$TMPFILE" ]]; then
    if [[ "$JSON" = true ]]; then
        echo "[]"
    else
        echo "No Kanaha apps found${ONLY:+ of kind $ONLY}."
        echo ""
        echo "Troubleshooting:"
        echo "  - Open each Kanaha app: it advertises itself only while its server runs"
        echo "    (after a reboot nothing runs until the app is opened)"
        echo "  - WiFi on, and on the same network as this computer"
        echo "  - Try: $(basename "$0") --ip <phone-ip>"
    fi
    exit 1
fi

if [[ "$JSON" = true ]]; then
    python3 -c '
import json, sys
out = []
for row in open(sys.argv[1]):
    kind, ip, port, name, model, manu, host, ok, err, extra, _ = row.rstrip("\n").split("\x1f")
    svc = {"camera": "CameraControlService", "audio": "AudioSearchService",
           "calcs": "FinancialBenchmarkService"}[kind]
    tls = {"camera": sys.argv[2], "calcs": sys.argv[3]}.get(kind, "")
    e = {"kind": kind, "name": name, "ip": ip, "port": int(port), "hostname": host,
         "model": model, "manufacturer": manu, "reachable": ok == "true",
         "tls_name": tls or ip, "url": "https://%s:%s/services/%s" % (tls or ip, port, svc)}
    if err: e["error"] = err
    e.update(json.loads(extra or "{}"))
    out.append(e)
print(json.dumps(out, indent=2))
' "$TMPFILE" "$(tls_name_of camera)" "$(tls_name_of calcs)"
else
    echo -e "\033[32mFound $(wc -l < "$TMPFILE") Kanaha app(s):\033[0m"
    echo ""
    while IFS="$US" read -r kind ip port name model manu host ok err extra summary; do
        tls=$(tls_name_of "$kind")
        if [[ "$model" = Unknown ]]; then
            printf '  %-7s %s\n' "$kind" "${name:-$ip}"
        else
            printf '  %-7s %s (%s %s)\n' "$kind" "${name:-$ip}" "$manu" "$model"
        fi
        echo "    Address:  $ip:$port"
        if [[ "$ok" = true ]]; then
            echo "    Status:   $summary"
        else
            echo -e "    Status:   \033[31mnot reachable\033[0m: $err"
        fi
        if [[ -n "$tls" ]]; then
            echo "    URL:      https://$tls:$port/services/$(service_of "$kind")  (curl --resolve $tls:$port:$ip)"
        else
            echo "    URL:      https://$ip:$port/services/$(service_of "$kind")"
        fi
        echo ""
    done < "$TMPFILE"
fi
