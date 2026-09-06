#!/bin/bash
#
# generate-edl.sh — Generate ffmpeg concat EDL from LTC timecode + audio cues
#
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025-2026 Robert Lazarski
#
# Replaces the magic values in parseLTC.sh (lines 79-98, 117-127) with
# a structured approach: LTC decoding determines camera sync, audio event
# detection determines edit points (start/stop/camera overrides).
#
# Architecture:
#   This script → kanaha-audio APIs (decodeLTC, detectAudioEvents, searchKeywords)
#     → generates ffmpeg concat EDL file
#     → ffmpeg -f concat -safe 0 -i edl.txt ... produces final video
#
# Inputs:
#   - Camera video files with LTC on audio ch1 (from Tentacle Sync units)
#   - Room recording WAV (from a second phone, e.g., Moto X4 via built-in mic)
#   - Configuration: transition interval, cue definitions
#
# Outputs:
#   - edl.txt (ffmpeg concat demuxer format)
#   - edl.json (structured JSON for further processing)
#
# Usage:
#   ./generate-edl.sh --config edl-config.json
#   ./generate-edl.sh --config edl-config.json --dry-run
#
# Config file format (edl-config.json):
# {
#   "kanaha_audio_host": "192.168.8.159:8443",
#   "ssl_dir": "~/repos/kanaha/kanaha-camera-app/app/src/main/assets/ssl",
#   "cameras": [
#     {"id": "cam1", "video_file": "q81.MOV", "ltc_channel": 1},
#     {"id": "cam2", "video_file": "q82.MOV", "ltc_channel": 1},
#     {"id": "cam3", "video_file": "q83.MOV", "ltc_channel": 1}
#   ],
#   "room_recording": "room.wav",
#   "transition_seconds": 20,
#   "first_camera": "cam1",
#   "fps": 29.97,
#   "start_cue": {"type": "audio_event", "event": "Saxophone"},
#   "stop_cue": {"type": "audio_event", "event": "Drum"},
#   "overrides": [
#     {"type": "keyword", "keyword": "next slide please", "camera": "cam3", "duration_seconds": 15}
#   ]
# }

set -euo pipefail

# --- Defaults ---
DRY_RUN=0
CONFIG_FILE=""
OUTPUT_DIR="."
CURL_TIMEOUT=300

# --- Parse args ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)   CONFIG_FILE="$2"; shift 2 ;;
        --dry-run)  DRY_RUN=1; shift ;;
        --output)   OUTPUT_DIR="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 --config <edl-config.json> [--dry-run] [--output <dir>]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ -z "$CONFIG_FILE" ]]; then
    echo "Error: --config is required"
    echo "Usage: $0 --config <edl-config.json>"
    exit 1
fi

if [[ ! -f "$CONFIG_FILE" ]]; then
    echo "Error: config file not found: $CONFIG_FILE"
    exit 1
fi

# --- Read config ---
HOST=$(CONFIG_FILE="$CONFIG_FILE" python3 -c "import json,os; c=json.load(open(os.environ['CONFIG_FILE'])); print(c['kanaha_audio_host'])")
SSL_DIR=$(CONFIG_FILE="$CONFIG_FILE" python3 -c "import json,os; c=json.load(open(os.environ['CONFIG_FILE'])); print(os.path.expanduser(c['ssl_dir']))")
FPS=$(CONFIG_FILE="$CONFIG_FILE" python3 -c "import json,os; c=json.load(open(os.environ['CONFIG_FILE'])); print(c.get('fps', 29.97))")
TRANSITION=$(CONFIG_FILE="$CONFIG_FILE" python3 -c "import json,os; c=json.load(open(os.environ['CONFIG_FILE'])); print(c.get('transition_seconds', 20))")
FIRST_CAM=$(CONFIG_FILE="$CONFIG_FILE" python3 -c "import json,os; c=json.load(open(os.environ['CONFIG_FILE'])); print(c.get('first_camera', 'cam1'))")
ROOM_WAV=$(CONFIG_FILE="$CONFIG_FILE" python3 -c "import json,os; c=json.load(open(os.environ['CONFIG_FILE'])); print(c.get('room_recording', ''))")

echo "=== Kanaha Audio EDL Generator ==="
echo "Host: $HOST"
echo "FPS: $FPS"
echo "Transition: ${TRANSITION}s"
echo "First camera: $FIRST_CAM"
echo ""

# --- Helper: curl to kanaha-audio ---
ka_curl() {
    local action="$1"
    local data="$2"
    # WARNING: -k disables peer AND hostname verification (--cacert is inert here);
    # a LAN peer can impersonate the phone. Known gap, see SECURITY.md. Fix needs
    # server certs with an IP SAN. Left as -k pending that cert change.
    curl -sk --http2 --max-time "$CURL_TIMEOUT" \
        --cert "$SSL_DIR/client.crt" \
        --key "$SSL_DIR/client.key" \
        --cacert "$SSL_DIR/ca.crt" \
        -H "Content-Type: application/json" \
        -d "$data" \
        "https://$HOST/services/AudioSearchService/$action"
}

mkdir -p "$OUTPUT_DIR"

# --- Step 1: Extract LTC audio from camera videos and decode ---
echo "--- Step 1: Decode LTC from cameras ---"

NUM_CAMERAS=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(len(c['cameras']))")
CAMERA_IDS=()
CAMERA_VIDEOS=()

for i in $(seq 0 $((NUM_CAMERAS - 1))); do
    CAM_ID=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['cameras'][$i]['id'])")
    CAM_VIDEO=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['cameras'][$i]['video_file'])")
    CAM_CHANNEL=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['cameras'][$i].get('ltc_channel', 1))")

    CAMERA_IDS+=("$CAM_ID")
    CAMERA_VIDEOS+=("$CAM_VIDEO")

    echo "  Camera $CAM_ID: $CAM_VIDEO (LTC channel $CAM_CHANNEL)"

    # Extract LTC audio channel from video to WAV
    LTC_WAV="$OUTPUT_DIR/${CAM_ID}_ltc.wav"
    if [[ ! -f "$LTC_WAV" ]]; then
        echo "    Extracting LTC audio..."
        ffmpeg -y -v error -i "$CAM_VIDEO" \
            -map 0:a:0 -af "pan=mono|c0=c$((CAM_CHANNEL-1))" \
            -ar 48000 -acodec pcm_s16le "$LTC_WAV"
    else
        echo "    Using existing $LTC_WAV"
    fi

    # Push to phone and decode LTC
    # For files already on phone, skip push
    REMOTE_PATH="/data/local/tmp/kanaha-audio/audio/$(basename "$LTC_WAV")"

    echo "    Decoding LTC..."
    LTC_JSON=$(ka_curl "decodeLTC" "{\"action\":\"decodeLTC\",\"audio_file\":\"$REMOTE_PATH\",\"channel\":1}")

    # Save LTC decode result
    echo "$LTC_JSON" > "$OUTPUT_DIR/${CAM_ID}_ltc.json"
    FRAMES=$(echo "$LTC_JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('total_frames',0))")
    FIRST_TC=$(echo "$LTC_JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('first_tc','?'))")
    LAST_TC=$(echo "$LTC_JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('last_tc','?'))")
    echo "    Decoded: $FRAMES frames, $FIRST_TC → $LAST_TC"
done
echo ""

# --- Step 2: Detect audio cues from room recording ---
echo "--- Step 2: Detect audio cues from room recording ---"

START_CUE_TC=""
STOP_CUE_TC=""
OVERRIDE_EVENTS="[]"

if [[ -n "$ROOM_WAV" ]]; then
    REMOTE_ROOM="/data/local/tmp/kanaha-audio/audio/$(basename "$ROOM_WAV")"

    # Detect start cue
    START_CUE_TYPE=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c.get('start_cue',{}).get('type',''))")
    if [[ "$START_CUE_TYPE" == "audio_event" ]]; then
        START_EVENT=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['start_cue']['event'])")
        echo "  Detecting start cue: $START_EVENT"
        DETECT_JSON=$(ka_curl "detectAudioEvents" "{\"action\":\"detectAudioEvents\",\"audio_file\":\"$REMOTE_ROOM\",\"events\":[\"$START_EVENT\"]}")
        echo "$DETECT_JSON" > "$OUTPUT_DIR/start_cue.json"
        START_CUE_MS=$(echo "$DETECT_JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
dets=d.get('detections',[])
if dets: print(dets[0]['start_ms'])
else: print('')
")
        if [[ -n "$START_CUE_MS" ]]; then
            echo "  Start cue found at ${START_CUE_MS}ms"
        else
            echo "  WARNING: Start cue '$START_EVENT' not detected"
        fi
    elif [[ "$START_CUE_TYPE" == "timecode" ]]; then
        START_CUE_TC=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['start_cue']['tc'])")
        echo "  Start cue: timecode $START_CUE_TC"
    fi

    # Detect stop cue
    STOP_CUE_TYPE=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c.get('stop_cue',{}).get('type',''))")
    if [[ "$STOP_CUE_TYPE" == "audio_event" ]]; then
        STOP_EVENT=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['stop_cue']['event'])")
        echo "  Detecting stop cue: $STOP_EVENT"
        DETECT_JSON=$(ka_curl "detectAudioEvents" "{\"action\":\"detectAudioEvents\",\"audio_file\":\"$REMOTE_ROOM\",\"events\":[\"$STOP_EVENT\"]}")
        echo "$DETECT_JSON" > "$OUTPUT_DIR/stop_cue.json"
        STOP_CUE_MS=$(echo "$DETECT_JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
dets=d.get('detections',[])
if dets: print(dets[-1]['end_ms'])
else: print('')
")
        if [[ -n "$STOP_CUE_MS" ]]; then
            echo "  Stop cue found at ${STOP_CUE_MS}ms"
        else
            echo "  WARNING: Stop cue '$STOP_EVENT' not detected"
        fi
    elif [[ "$STOP_CUE_TYPE" == "timecode" ]]; then
        STOP_CUE_TC=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['stop_cue']['tc'])")
        echo "  Stop cue: timecode $STOP_CUE_TC"
    fi

    # Detect override triggers (keyword searches)
    NUM_OVERRIDES=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(len(c.get('overrides',[])))")
    if [[ "$NUM_OVERRIDES" -gt 0 ]]; then
        echo "  Detecting override triggers..."
        for oi in $(seq 0 $((NUM_OVERRIDES - 1))); do
            OV_TYPE=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['overrides'][$oi]['type'])")
            if [[ "$OV_TYPE" == "keyword" ]]; then
                OV_KEYWORD=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['overrides'][$oi]['keyword'])")
                OV_CAM=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['overrides'][$oi]['camera'])")
                OV_DUR=$(python3 -c "import json; c=json.load(open('$CONFIG_FILE')); print(c['overrides'][$oi]['duration_seconds'])")

                echo "    Searching for keyword: '$OV_KEYWORD' → $OV_CAM for ${OV_DUR}s"
                SEARCH_JSON=$(ka_curl "searchKeywords" "{\"action\":\"searchKeywords\",\"audio_file\":\"$REMOTE_ROOM\",\"keywords\":[\"$OV_KEYWORD\"]}")
                echo "$SEARCH_JSON" > "$OUTPUT_DIR/override_${oi}.json"

                MATCHES=$(echo "$SEARCH_JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('total_matches',0))")
                echo "    Found $MATCHES matches"
            fi
        done
    fi
fi
echo ""

# --- Step 3: Generate EDL ---
echo "--- Step 3: Generate EDL ---"

python3 -c "
import json, sys, os

config = json.load(open('$CONFIG_FILE'))
output_dir = '$OUTPUT_DIR'
fps = float('$FPS')
transition_s = int('$TRANSITION')
first_cam = '$FIRST_CAM'

# Load LTC data for each camera
cameras = config['cameras']
ltc_data = {}
for cam in cameras:
    ltc_file = os.path.join(output_dir, cam['id'] + '_ltc.json')
    if os.path.exists(ltc_file):
        ltc_data[cam['id']] = json.load(open(ltc_file))

# Build timecode → sample position lookup for each camera
def tc_to_seconds(tc_str):
    \"\"\"Convert HH:MM:SS:FF to seconds\"\"\"
    parts = tc_str.split(':')
    if len(parts) == 4:
        h, m, s, f = int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3])
        return h * 3600 + m * 60 + s + f / fps
    return 0

def seconds_to_hhmmss(secs):
    \"\"\"Convert seconds to HH:MM:SS.mmm\"\"\"
    h = int(secs // 3600)
    m = int((secs % 3600) // 60)
    s = secs % 60
    return f'{h:02d}:{m:02d}:{s:06.3f}'

def tc_to_hhmmssff(tc_str):
    \"\"\"Format TC as HH:MM:SS:FF\"\"\"
    return tc_str  # already in this format

# For each camera, build a map from timecode seconds → file position seconds
# The file position is: sample_start / sample_rate
cam_tc_maps = {}
for cam in cameras:
    cid = cam['id']
    if cid not in ltc_data or not ltc_data[cid].get('success'):
        print(f'WARNING: No LTC data for {cid}', file=sys.stderr)
        continue

    ld = ltc_data[cid]
    sr = ld['sample_rate']
    frames = ld.get('frames', [])

    # Build sorted list of (tc_seconds, file_position_seconds)
    tc_map = []
    for fr in frames:
        tc_s = tc_to_seconds(fr['tc'])
        file_s = fr['sample_start'] / sr
        tc_map.append((tc_s, file_s))

    cam_tc_maps[cid] = tc_map

def lookup_file_position(cam_id, tc_seconds):
    \"\"\"Given a timecode in seconds, find the file position for that camera.
    Uses LTC frame data for sample-accurate lookup (no magicNumberOffset needed).\"\"\"
    if cam_id not in cam_tc_maps:
        return None
    tc_map = cam_tc_maps[cam_id]
    if not tc_map:
        return None

    # Binary search for nearest timecode
    best = None
    best_diff = float('inf')
    for tc_s, file_s in tc_map:
        diff = abs(tc_s - tc_seconds)
        if diff < best_diff:
            best_diff = diff
            best = file_s
    return best

# Determine start/stop from cues
start_tc_s = None
stop_tc_s = None

# Check for audio event cues
start_cue = config.get('start_cue', {})
stop_cue = config.get('stop_cue', {})

if start_cue.get('type') == 'audio_event':
    cue_file = os.path.join(output_dir, 'start_cue.json')
    if os.path.exists(cue_file):
        cue_data = json.load(open(cue_file))
        dets = cue_data.get('detections', [])
        if dets:
            # Room recording timestamp — need to map to LTC timecode
            # For now, use the room recording's first LTC frame as reference
            start_cue_ms = dets[0]['start_ms']
            # The room recording start time aligns with the first camera's LTC
            # This mapping requires the room recording to have started at a known time
            # For simplicity: treat room recording ms offset as offset from first LTC frame
            if cam_tc_maps:
                first_cam_data = ltc_data.get(cameras[0]['id'], {})
                if first_cam_data.get('frames'):
                    first_tc_s = tc_to_seconds(first_cam_data['frames'][0]['tc'])
                    start_tc_s = first_tc_s + start_cue_ms / 1000.0

if start_cue.get('type') == 'timecode':
    start_tc_s = tc_to_seconds(start_cue['tc'])

if stop_cue.get('type') == 'audio_event':
    cue_file = os.path.join(output_dir, 'stop_cue.json')
    if os.path.exists(cue_file):
        cue_data = json.load(open(cue_file))
        dets = cue_data.get('detections', [])
        if dets:
            stop_cue_ms = dets[-1]['end_ms']
            if cam_tc_maps:
                first_cam_data = ltc_data.get(cameras[0]['id'], {})
                if first_cam_data.get('frames'):
                    first_tc_s = tc_to_seconds(first_cam_data['frames'][0]['tc'])
                    stop_tc_s = first_tc_s + stop_cue_ms / 1000.0

if stop_cue.get('type') == 'timecode':
    stop_tc_s = tc_to_seconds(stop_cue['tc'])

# Fallback: use LTC range from first camera
if start_tc_s is None and cam_tc_maps:
    first_cam_data = ltc_data.get(cameras[0]['id'], {})
    if first_cam_data.get('first_tc'):
        start_tc_s = tc_to_seconds(first_cam_data['first_tc'])

if stop_tc_s is None and cam_tc_maps:
    first_cam_data = ltc_data.get(cameras[0]['id'], {})
    if first_cam_data.get('last_tc'):
        stop_tc_s = tc_to_seconds(first_cam_data['last_tc'])

if start_tc_s is None or stop_tc_s is None:
    print('ERROR: Cannot determine start/stop times', file=sys.stderr)
    sys.exit(1)

print(f'EDL range: {seconds_to_hhmmss(start_tc_s)} → {seconds_to_hhmmss(stop_tc_s)}')
print(f'Duration: {stop_tc_s - start_tc_s:.1f}s')

# Load override triggers
overrides = []
for oi, ov in enumerate(config.get('overrides', [])):
    ov_file = os.path.join(output_dir, f'override_{oi}.json')
    if os.path.exists(ov_file):
        ov_data = json.load(open(ov_file))
        matches = ov_data.get('matches', [])
        for match in matches:
            # Map room recording ms → timecode seconds
            first_cam_data = ltc_data.get(cameras[0]['id'], {})
            if first_cam_data.get('frames'):
                first_tc_s_ref = tc_to_seconds(first_cam_data['frames'][0]['tc'])
                trigger_tc_s = first_tc_s_ref + match['start_ms'] / 1000.0
                overrides.append({
                    'tc_seconds': trigger_tc_s,
                    'camera': ov['camera'],
                    'duration_seconds': ov['duration_seconds'],
                    'keyword': ov.get('keyword', '')
                })

overrides.sort(key=lambda x: x['tc_seconds'])
if overrides:
    print(f'Overrides: {len(overrides)} keyword triggers')

# Generate EDL entries
# Alternate cameras every transition_seconds, with override interruptions
cam_order = [c['id'] for c in cameras if c['id'] != first_cam]
cam_order.insert(0, first_cam)
# Only alternate between first two cameras (third is override-only)
alt_cams = [c['id'] for c in cameras[:2]]
if first_cam in alt_cams:
    alt_cams.remove(first_cam)
    alt_cams.insert(0, first_cam)

edl_entries = []
current_tc = start_tc_s
cam_idx = 0

while current_tc < stop_tc_s:
    # Check if an override fires at this point
    active_override = None
    for ov in overrides:
        if ov['tc_seconds'] >= current_tc and ov['tc_seconds'] < current_tc + transition_s:
            active_override = ov
            break

    if active_override and active_override['tc_seconds'] > current_tc:
        # Normal cut before the override
        cut_end = min(active_override['tc_seconds'], stop_tc_s)
        cam_id = alt_cams[cam_idx % len(alt_cams)]

        # Look up file positions from LTC (sample-accurate, no magicNumberOffset)
        in_pos = lookup_file_position(cam_id, current_tc)
        out_pos = lookup_file_position(cam_id, cut_end)

        if in_pos is not None and out_pos is not None:
            video_file = next(c['video_file'] for c in cameras if c['id'] == cam_id)
            edl_entries.append({
                'camera': cam_id,
                'video_file': video_file,
                'inpoint': seconds_to_hhmmss(in_pos),
                'outpoint': seconds_to_hhmmss(out_pos),
                'tc_in': seconds_to_hhmmss(current_tc),
                'tc_out': seconds_to_hhmmss(cut_end),
                'type': 'normal'
            })

        current_tc = cut_end

        # Insert override camera
        ov_end = min(current_tc + active_override['duration_seconds'], stop_tc_s)
        ov_cam = active_override['camera']
        in_pos = lookup_file_position(ov_cam, current_tc)
        out_pos = lookup_file_position(ov_cam, ov_end)

        if in_pos is not None and out_pos is not None:
            video_file = next(c['video_file'] for c in cameras if c['id'] == ov_cam)
            edl_entries.append({
                'camera': ov_cam,
                'video_file': video_file,
                'inpoint': seconds_to_hhmmss(in_pos),
                'outpoint': seconds_to_hhmmss(out_pos),
                'tc_in': seconds_to_hhmmss(current_tc),
                'tc_out': seconds_to_hhmmss(ov_end),
                'type': 'override',
                'trigger': active_override.get('keyword', '')
            })

        current_tc = ov_end
        cam_idx += 1
    else:
        # Normal alternating cut
        cut_end = min(current_tc + transition_s, stop_tc_s)
        cam_id = alt_cams[cam_idx % len(alt_cams)]

        in_pos = lookup_file_position(cam_id, current_tc)
        out_pos = lookup_file_position(cam_id, cut_end)

        if in_pos is not None and out_pos is not None:
            video_file = next(c['video_file'] for c in cameras if c['id'] == cam_id)
            edl_entries.append({
                'camera': cam_id,
                'video_file': video_file,
                'inpoint': seconds_to_hhmmss(in_pos),
                'outpoint': seconds_to_hhmmss(out_pos),
                'tc_in': seconds_to_hhmmss(current_tc),
                'tc_out': seconds_to_hhmmss(cut_end),
                'type': 'normal'
            })

        current_tc = cut_end
        cam_idx += 1

# Write ffmpeg concat EDL
edl_text_lines = []
for entry in edl_entries:
    edl_text_lines.append(f\"file '{entry['video_file']}'\")
    edl_text_lines.append(f\"inpoint {entry['inpoint']}\")
    edl_text_lines.append(f\"outpoint {entry['outpoint']}\")

edl_text = '\n'.join(edl_text_lines) + '\n'

edl_path = os.path.join(output_dir, 'edl.txt')
with open(edl_path, 'w') as f:
    f.write(edl_text)
print(f'Wrote {edl_path} ({len(edl_entries)} cuts)')

# Write structured JSON
edl_json = {
    'success': True,
    'edl_format': 'ffmpeg_concat',
    'total_cuts': len(edl_entries),
    'fps': fps,
    'start_tc': seconds_to_hhmmss(start_tc_s),
    'stop_tc': seconds_to_hhmmss(stop_tc_s),
    'duration_seconds': round(stop_tc_s - start_tc_s, 3),
    'edl': edl_entries,
    'edl_text': edl_text
}

json_path = os.path.join(output_dir, 'edl.json')
with open(json_path, 'w') as f:
    json.dump(edl_json, f, indent=2)
print(f'Wrote {json_path}')

# Print summary
print()
print('=== EDL Summary ===')
for i, entry in enumerate(edl_entries):
    marker = ' [OVERRIDE]' if entry.get('type') == 'override' else ''
    print(f'  {i+1:3d}. {entry[\"camera\"]:5s} {entry[\"tc_in\"]} → {entry[\"tc_out\"]}{marker}')

print()
print(f'Total cuts: {len(edl_entries)}')
print(f'Duration: {stop_tc_s - start_tc_s:.1f}s')
print()
print('To assemble video:')
print(f'  ffmpeg -f concat -safe 0 -i {edl_path} -c copy output.mov')
"

echo ""
echo "=== EDL generation complete ==="
