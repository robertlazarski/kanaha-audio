#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025-2026 Robert Lazarski
set -euo pipefail
#
# Point Kanaha Audio's voice path at a Kanaha Calcs phone -- a laptop prep step.
#
# Usage: kanaha-voice-setup.sh --serial <audio adb serial> --books <kanaha-books.json>
#                              [--calcs <ip>] [--port 8444] [--name calcs.local]
#                              [--camera <ip>] [--camera-port 8443] [--camera-name camera.local]
#                              [--no-autostart]
#
# Without --calcs the calcs phone is found by mDNS: the _https._tcp service
# whose TXT record carries api=kanaha-calcs. Never by its .local hostname,
# which Android randomizes per boot. The address found is written into the
# audio phone as a static voice/voice.json; the request path never looks it
# up again. Run it again if the calcs phone's address changes.
#
# --name is the name the calcs certificate carries (its SAN), checked by the
# audio phone on every connection. The calcs provisioning names it calcs.local.
#
# The camera defaults to the calcs phone (both apps run on the Pixel); its
# certificate names it camera.local.
#
# The voice loop starts listening when the audio server starts, unless
# --no-autostart. Start the server from the app, so the app is in the
# foreground: Android gives a background app a silent microphone.

SERIAL="" BOOKS="" CALCS="" PORT=8444 NAME="calcs.local" AUTOSTART=true
CAMERA="" CAMERA_PORT=8443 CAMERA_NAME="camera.local"
while [[ $# -gt 0 ]]; do
    case $1 in
        --serial) SERIAL="$2"; shift 2 ;;
        --books) BOOKS="$2"; shift 2 ;;
        --calcs) CALCS="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --name) NAME="$2"; shift 2 ;;
        --no-autostart) AUTOSTART=false; shift ;;
        --camera) CAMERA="$2"; shift 2 ;;
        --camera-port) CAMERA_PORT="$2"; shift 2 ;;
        --camera-name) CAMERA_NAME="$2"; shift 2 ;;
        -h|--help) sed -n '5,26p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ -n "$SERIAL" && -f "$BOOKS" ]] || { echo "need --serial and an existing --books file" >&2; exit 2; }
python3 -c "import json,sys; json.load(open(sys.argv[1]))['books']" "$BOOKS" \
    || { echo "$BOOKS is not a books file" >&2; exit 2; }

if [[ -z "$CALCS" ]]; then
    command -v avahi-browse >/dev/null || { echo "no avahi-browse; pass --calcs <ip>" >&2; exit 2; }
    # resolved lines: =;iface;proto;name;type;domain;host;address;port;"txt"...
    CALCS=$(avahi-browse -rpt _https._tcp 2>/dev/null \
        | awk -F';' '$1 == "=" && $3 == "IPv4" && /api=kanaha-calcs/ { print $8 ":" $9; exit }')
    [[ -n "$CALCS" ]] || { echo "no kanaha-calcs service on the network; pass --calcs <ip>" >&2; exit 1; }
    PORT="${CALCS##*:}"; CALCS="${CALCS%%:*}"
    echo "found Kanaha Calcs at $CALCS:$PORT by mDNS"
fi

[[ -n "$CAMERA" ]] || CAMERA="$CALCS"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
printf '{"calcs": {"host": "%s", "port": %d, "verify_name": "%s"},\n "camera": {"host": "%s", "port": %d, "verify_name": "%s"},\n "loop": {"autostart": %s, "clip_secs": 6, "spec_secs": 8}}\n' \
    "$CALCS" "$PORT" "$NAME" "$CAMERA" "$CAMERA_PORT" "$CAMERA_NAME" "$AUTOSTART" > "$TMP/voice.json"

A=(adb -s "$SERIAL")
"${A[@]}" push "$TMP/voice.json" /data/local/tmp/kanaha-voice.json >/dev/null
"${A[@]}" push "$BOOKS" /data/local/tmp/kanaha-books.json >/dev/null
"${A[@]}" shell "run-as org.kanaha.audio mkdir -p files/voice \
  && run-as org.kanaha.audio cp /data/local/tmp/kanaha-voice.json files/voice/voice.json \
  && run-as org.kanaha.audio cp /data/local/tmp/kanaha-books.json files/voice/kanaha-books.json \
  && run-as org.kanaha.audio chmod 700 files/voice \
  && run-as org.kanaha.audio chmod 600 files/voice/voice.json files/voice/kanaha-books.json; \
  rm -f /data/local/tmp/kanaha-voice.json /data/local/tmp/kanaha-books.json"
echo "voice path on $SERIAL now calls $CALCS:$PORT (certificate name $NAME)"
echo "camera phrases call $CAMERA:$CAMERA_PORT (certificate name $CAMERA_NAME)"
echo "voice loop autostart: $AUTOSTART (takes effect when the audio server next starts)"
echo "if the service is already running, send voiceReset so the voice path rereads them"
