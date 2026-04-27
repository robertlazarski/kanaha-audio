#!/bin/bash
#
# Kanaha Audio Workflow Script
# Room audio recording + analysis + EDL generation + video assembly
#
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2025-2026 Robert Lazarski
#
# Demonstrates the full Kanaha Audio pipeline:
#   1. Discover devices (Kanaha Audio on Moto X4, Kanaha Camera on Pixel)
#   2. Start coordinated recording on both devices
#   3. Analyze room audio: keyword search (whisper) + event detection (YAMNet)
#   4. Generate EDL (ffmpeg concat format) from detected cues
#   5. Assemble final video with slide inserts + audio fade
#
# Prerequisites:
#   - Kanaha Audio running on room mic device (e.g., Moto X4)
#   - Kanaha Camera running on video device (e.g., Pixel 10 Pro XL)
#   - Both devices on the same WiFi network
#   - mTLS certificates deployed
#   - Whisper model and YAMNet model on the audio device
#   - A slide image (slide.jpg) for "next slide please" inserts
#
# Usage:
#   ./test-audio-workflow.sh [command] [options]
#
# Commands:
#   status           Check both devices
#   record           Start coordinated recording
#   stop             Stop both recordings
#   analyze          Analyze room audio for cues
#   edl              Generate EDL from analysis
#   assemble         Assemble final video from EDL
#   workflow         Run full pipeline (record → analyze → edl → assemble)
#
# Options:
#   --duration <sec>       Recording duration (default: 60)
#   --audio-device <addr>  Audio device address (default: mDNS discovery)
#   --video-device <addr>  Video device address (default: mDNS discovery)
#   --slide <path>         Slide image for keyword triggers (default: slide.jpg)
#   --slide-duration <sec> How long to show each slide (default: 3)
#   --output <dir>         Output directory (default: ./output)
#   --start-event <name>   YAMNet event to start video (default: Music)
#   --stop-event <name>    YAMNet event to end video (default: Music)
#   --keyword <phrase>     Keyword to trigger slide insert (default: "next slide please")
#   --use-ip               Use IP addresses instead of mDNS
#   --verbose              Show full API responses
#
# Example — full workflow with Coltrane/Roach audio cues:
#   ./test-audio-workflow.sh workflow --duration 120 \
#     --start-event Music --stop-event Music \
#     --keyword "next slide please" --slide slide.jpg
#

set -e

# Configuration
SSL=$HOME/repos/kanaha/kanaha-camera-app/app/src/main/assets/ssl
AUDIO_PORT=8443
VIDEO_PORT=8443

# Default settings
DURATION=60
AUDIO_ADDR=""
VIDEO_ADDR=""
SLIDE_IMAGE="slide.jpg"
SLIDE_DURATION=3
OUTPUT_DIR="./output"
START_EVENT="Music"
STOP_EVENT="Music"
KEYWORD="next slide please"
USE_IP=false
VERBOSE=false
COMMAND="status"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        status|record|stop|analyze|edl|assemble|workflow)
            COMMAND="$1"; shift ;;
        --duration) DURATION="$2"; shift 2 ;;
        --audio-device) AUDIO_ADDR="$2"; shift 2 ;;
        --video-device) VIDEO_ADDR="$2"; shift 2 ;;
        --slide) SLIDE_IMAGE="$2"; shift 2 ;;
        --slide-duration) SLIDE_DURATION="$2"; shift 2 ;;
        --output) OUTPUT_DIR="$2"; shift 2 ;;
        --start-event) START_EVENT="$2"; shift 2 ;;
        --stop-event) STOP_EVENT="$2"; shift 2 ;;
        --keyword) KEYWORD="$2"; shift 2 ;;
        --use-ip) USE_IP=true; shift ;;
        --verbose|-v) VERBOSE=true; shift ;;
        --help|-h)
            head -48 "$0" | tail -46
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

mkdir -p "$OUTPUT_DIR"

# --- Helper functions ---

log() { echo -e "${GREEN}[kanaha]${NC} $*"; }
warn() { echo -e "${YELLOW}[kanaha]${NC} $*"; }
err() { echo -e "${RED}[kanaha]${NC} $*"; }

# API call to Kanaha Audio device
audio_api() {
    local action=$1
    local data=$2
    local timeout=${3:-15}
    # -k: skip hostname verification (self-signed cert CN doesn't include device IP)
    # --cacert: still verifies the CA chain — not disabling TLS, just hostname matching
    curl -sk --http2 --max-time "$timeout" \
        --cert "$SSL/client.crt" --key "$SSL/client.key" --cacert "$SSL/ca.crt" \
        -H "Content-Type: application/json" \
        -d "$data" \
        "https://${AUDIO_ADDR}:${AUDIO_PORT}/services/AudioSearchService/${action}"
}

# API call to Kanaha Camera device
video_api() {
    local action=$1
    local data=$2
    local timeout=${3:-15}
    curl -sk --http2 --max-time "$timeout" \
        --cert "$SSL/client.crt" --key "$SSL/client.key" --cacert "$SSL/ca.crt" \
        -H "Content-Type: application/json" \
        -d "$data" \
        "https://${VIDEO_ADDR}:${VIDEO_PORT}/services/CameraControlService/${action}"
}

# Discover devices via mDNS or use provided addresses
discover_devices() {
    if [[ -n "$AUDIO_ADDR" && -n "$VIDEO_ADDR" ]]; then
        return
    fi

    log "Discovering devices..."

    # Try kanaha-audio-discover.sh if available
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    if [[ -x "$SCRIPT_DIR/kanaha-audio-discover.sh" && -z "$AUDIO_ADDR" ]]; then
        AUDIO_ADDR=$("$SCRIPT_DIR/kanaha-audio-discover.sh" --json 2>/dev/null | \
            python3 -c "import sys,json; d=json.load(sys.stdin); print(d[0]['ip'])" 2>/dev/null || true)
    fi

    # Try kanaha-discover.sh for camera
    CAMERA_DISCOVER="$HOME/repos/kanaha/tools/kanaha-discover.sh"
    if [[ -x "$CAMERA_DISCOVER" && -z "$VIDEO_ADDR" ]]; then
        VIDEO_ADDR=$("$CAMERA_DISCOVER" --json 2>/dev/null | \
            python3 -c "import sys,json; d=json.load(sys.stdin); print(d[0]['ip'])" 2>/dev/null || true)
    fi

    if [[ -z "$AUDIO_ADDR" ]]; then
        err "Audio device not found. Use --audio-device <ip>"
        exit 1
    fi
    if [[ -z "$VIDEO_ADDR" ]]; then
        warn "Video device not found. Use --video-device <ip> (audio-only mode)"
    fi

    log "Audio device: $AUDIO_ADDR"
    [[ -n "$VIDEO_ADDR" ]] && log "Video device: $VIDEO_ADDR"
}

# --- Commands ---

cmd_status() {
    log "Checking devices..."

    echo -e "\n${BLUE}=== Audio Device ($AUDIO_ADDR) ===${NC}"
    local audio_status
    audio_status=$(audio_api "getStatus" '{"action":"getStatus"}')
    echo "$audio_status" | python3 -c "
import sys,json
d=json.load(sys.stdin)
w=d.get('whisper',{})
print(f'  Whisper: {\"loaded (\" + w.get(\"current_model\",\"?\") + \")\" if w.get(\"model_loaded\") else \"not loaded\"}')
print(f'  YAMNet: {\"ready\" if d.get(\"yamnet_ready\") else \"not ready\"}')
print(f'  Recording: {d.get(\"recording_active\")}')
" 2>/dev/null || echo "  (unreachable)"

    if [[ -n "$VIDEO_ADDR" ]]; then
        echo -e "\n${BLUE}=== Video Device ($VIDEO_ADDR) ===${NC}"
        local video_status
        video_status=$(video_api "getStatus" '{"action":"getStatus"}')
        echo "$video_status" | python3 -c "
import sys,json
d=json.load(sys.stdin).get('status',{})
print(f'  Camera: {\"ready\" if d.get(\"camera_available\") else \"not available\"}')
print(f'  Preview: {d.get(\"preview_active\")}')
print(f'  Video mode: {d.get(\"video_mode\")}')
print(f'  Recording: {d.get(\"is_recording\")}')
print(f'  Battery: {d.get(\"battery_level\")}%')
" 2>/dev/null || echo "  (unreachable)"
    fi
}

cmd_record() {
    log "Starting coordinated recording (${DURATION}s)..."

    # Start audio recording on Moto
    log "Starting room audio on $AUDIO_ADDR..."
    local clip="room_$(date +%Y%m%d_%H%M%S)"
    local audio_resp
    audio_resp=$(audio_api "startRecording" \
        "{\"action\":\"startRecording\",\"clip_name\":\"$clip\",\"sample_rate\":16000}")
    echo "$audio_resp" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'  {d}')" 
    # Start video recording on Pixel
    if [[ -n "$VIDEO_ADDR" ]]; then
        log "Starting video on $VIDEO_ADDR..."
        local video_resp
        video_resp=$(video_api "startRecording" '{"action":"startRecording"}' 20)
        echo "$video_resp" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'  {d}')" 2>/dev/null
    fi

    log "Both devices recording. Waiting ${DURATION}s..."
    sleep "$DURATION"

    cmd_stop
}

cmd_stop() {
    log "Stopping recordings..."

    local audio_resp
    audio_resp=$(audio_api "stopRecording" '{"action":"stopRecording"}')
    echo "$audio_resp" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'  Audio: {d.get(\"clip_name\",\"?\")} — {d.get(\"duration_ms\",0)/1000:.1f}s, {d.get(\"file_size_bytes\",0)/1024:.0f} KB')
" 
    if [[ -n "$VIDEO_ADDR" ]]; then
        local video_resp
        video_resp=$(video_api "stopRecording" '{"action":"stopRecording"}' 20)
        echo "$video_resp" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'  Video: {d}')" 2>/dev/null
    fi
}

cmd_analyze() {
    log "Analyzing room audio..."

    # Find the most recent room recording
    local recordings
    recordings=$(audio_api "listRecordings" '{"action":"listRecordings"}')
    local latest
    latest=$(echo "$recordings" | python3 -c "
import sys,json
d=json.load(sys.stdin)
recs=[r for r in d.get('recordings',[]) if r['name'].startswith('room_')]
if recs: print(recs[-1]['name'])
" 2>/dev/null)

    if [[ -z "$latest" ]]; then
        err "No room recordings found"
        exit 1
    fi

    log "Analyzing: $latest"
    local models_dir
    models_dir=$(audio_api "getStatus" '{"action":"getStatus"}' | \
        python3 -c "import sys,json; print(json.load(sys.stdin)['whisper']['models_dir'])" 2>/dev/null)
    local audio_dir="${models_dir}/../audio"
    local audio_path="${audio_dir}/${latest}"

    # Load whisper if needed
    local model_loaded
    model_loaded=$(audio_api "getStatus" '{"action":"getStatus"}' | \
        python3 -c "import sys,json; print(json.load(sys.stdin)['whisper']['model_loaded'])" 2>/dev/null)
    if [[ "$model_loaded" != "True" ]]; then
        log "Loading whisper model..."
        audio_api "loadModel" '{"action":"loadModel","model":"base.en"}' 60 > /dev/null
    fi

    # Search for keywords
    echo -e "\n${BLUE}=== Keyword Search: '$KEYWORD' ===${NC}"
    local kw_result
    kw_result=$(audio_api "searchKeywords" \
        "{\"action\":\"searchKeywords\",\"audio_file\":\"$audio_path\",\"keywords\":[\"$KEYWORD\"]}" 600)
    echo "$kw_result" > "$OUTPUT_DIR/keywords.json"
    echo "$kw_result" | python3 -c "
import sys,json
d=json.load(sys.stdin)
print(f'  Matches: {d.get(\"total_matches\",0)}')
for m in d.get('matches',[]):
    print(f'    {m[\"start_ms\"]/1000:.1f}s — {m[\"end_ms\"]/1000:.1f}s (conf: {m[\"confidence\"]})')
print(f'  Processing: {d.get(\"processing_time_ms\",0)/1000:.1f}s')
" 
    # Detect audio events
    echo -e "\n${BLUE}=== Audio Event Detection ===${NC}"
    local events_result
    events_result=$(audio_api "detectAudioEvents" \
        "{\"action\":\"detectAudioEvents\",\"audio_file\":\"$audio_path\",\"events\":[\"$START_EVENT\",\"$STOP_EVENT\",\"Speech\"]}" 120)
    echo "$events_result" > "$OUTPUT_DIR/events.json"
    echo "$events_result" | python3 -c "
import sys,json
d=json.load(sys.stdin)
events={}
for det in d.get('detections',[]):
    ev=det['event']
    if ev not in events: events[ev]=[]
    events[ev].append(det)
for ev,dets in sorted(events.items()):
    print(f'  {ev}: {len(dets)} detections')
    if dets:
        print(f'    first: {dets[0][\"start_ms\"]/1000:.1f}s, last: {dets[-1][\"end_ms\"]/1000:.1f}s')
" 
    log "Analysis saved to $OUTPUT_DIR/"
}

cmd_edl() {
    log "Generating EDL..."

    if [[ ! -f "$OUTPUT_DIR/keywords.json" || ! -f "$OUTPUT_DIR/events.json" ]]; then
        err "Run 'analyze' first"
        exit 1
    fi

    # Create slide video if image exists
    if [[ -f "$SLIDE_IMAGE" ]]; then
        log "Creating ${SLIDE_DURATION}s slide video from $SLIDE_IMAGE"
        ffmpeg -y -v error -loop 1 -i "$SLIDE_IMAGE" \
            -f lavfi -i anullsrc=r=48000:cl=mono \
            -c:v libx264 -crf 23 -pix_fmt yuv420p -r 30 \
            -c:a aac -b:a 128k \
            -t "$SLIDE_DURATION" -shortest \
            "$OUTPUT_DIR/slide.mp4"
    fi

    # Generate EDL using Python
    python3 << PYEOF
import json, os

output_dir = "$OUTPUT_DIR"
slide_dur = $SLIDE_DURATION
slide_file = os.path.join(output_dir, "slide.mp4") if os.path.exists(os.path.join(output_dir, "slide.mp4")) else None

# Load analysis results
kw = json.load(open(os.path.join(output_dir, "keywords.json")))
ev = json.load(open(os.path.join(output_dir, "events.json")))

# Find music start/end
events = {}
for det in ev.get("detections", []):
    e = det["event"]
    if e not in events: events[e] = []
    events[e].append(det)

music_dets = events.get("$START_EVENT", [])
music_start = music_dets[0]["start_ms"] / 1000 if music_dets else 0
music_end = music_dets[-1]["end_ms"] / 1000 if music_dets else 0

# Keyword triggers
matches = kw.get("matches", [])
# Deduplicate: skip matches within 8s of each other
filtered = []
for m in matches:
    if not filtered or m["start_ms"] - filtered[-1]["start_ms"] > 8000:
        filtered.append(m)

print(f"Music: {music_start:.1f}s → {music_end:.1f}s")
print(f"Keywords: {len(filtered)} (from {len(matches)} raw matches)")

# NOTE: video_file must be set to the actual video path
# For this script, we use a placeholder — replace with actual path
video_file = "VIDEO_FILE_PLACEHOLDER"
print(f"\\nIMPORTANT: Edit {output_dir}/edl.txt to set the video file path")
print(f"  Replace VIDEO_FILE_PLACEHOLDER with your video file")

# Build EDL
edl_segments = []

# Segment 1: video from music start to first keyword
if filtered:
    first_kw = filtered[0]["start_ms"] / 1000
    edl_segments.append(("video", music_start, first_kw))

    # For each keyword: insert slide, then video until next keyword or end
    for i, m in enumerate(filtered):
        edl_segments.append(("slide", 0, slide_dur))
        resume = m["start_ms"] / 1000 + slide_dur
        if i + 1 < len(filtered):
            next_kw = filtered[i + 1]["start_ms"] / 1000
            if next_kw > resume:
                edl_segments.append(("video", resume, next_kw))
        else:
            if music_end > resume:
                edl_segments.append(("video", resume, music_end))
else:
    edl_segments.append(("video", music_start, music_end))

# Write EDL
lines = []
for seg_type, start, end in edl_segments:
    if seg_type == "video":
        lines.append(f"file '{video_file}'")
        h1, m1 = int(start // 3600), int((start % 3600) // 60)
        s1 = start % 60
        h2, m2 = int(end // 3600), int((end % 3600) // 60)
        s2 = end % 60
        lines.append(f"inpoint {h1:02d}:{m1:02d}:{s1:06.3f}")
        lines.append(f"outpoint {h2:02d}:{m2:02d}:{s2:06.3f}")
    elif seg_type == "slide" and slide_file:
        lines.append(f"file '{slide_file}'")

edl_text = "\\n".join(lines) + "\\n"
with open(os.path.join(output_dir, "edl.txt"), "w") as f:
    f.write(edl_text)

print(f"\\nEDL ({len(edl_segments)} segments):")
for i, (t, s, e) in enumerate(edl_segments):
    if t == "video":
        print(f"  {i+1}. Video {s:.1f}s → {e:.1f}s ({e-s:.0f}s)")
    else:
        print(f"  {i+1}. Slide ({slide_dur}s)")

print(f"\\nWrote {output_dir}/edl.txt")

# Save metadata
meta = {
    "music_start": music_start, "music_end": music_end,
    "keywords": [{"time": m["start_ms"]/1000, "conf": m["confidence"]} for m in filtered],
    "segments": [{"type": t, "start": s, "end": e} for t, s, e in edl_segments]
}
with open(os.path.join(output_dir, "edl_meta.json"), "w") as f:
    json.dump(meta, f, indent=2)
PYEOF
}

cmd_assemble() {
    log "Assembling final video..."

    if [[ ! -f "$OUTPUT_DIR/edl.txt" ]]; then
        err "Run 'edl' first"
        exit 1
    fi

    if grep -q "VIDEO_FILE_PLACEHOLDER" "$OUTPUT_DIR/edl.txt"; then
        err "Edit $OUTPUT_DIR/edl.txt and replace VIDEO_FILE_PLACEHOLDER with your video path"
        exit 1
    fi

    # Calculate total duration for fade
    local total_dur
    total_dur=$(python3 -c "
import json
meta = json.load(open('$OUTPUT_DIR/edl_meta.json'))
total = sum(s['end'] - s['start'] for s in meta['segments'])
print(f'{total:.0f}')
" 2>/dev/null)
    local fade_start=$((total_dur - 3))

    log "Total duration: ${total_dur}s, audio fade out at ${fade_start}s"

    ffmpeg -y -v error \
        -f concat -safe 0 -i "$OUTPUT_DIR/edl.txt" \
        -c:v libx264 -crf 23 -preset fast \
        -c:a aac -b:a 128k \
        -af "afade=t=in:st=0:d=2,afade=t=out:st=${fade_start}:d=3" \
        -movflags +faststart \
        "$OUTPUT_DIR/final.mp4"

    log "Output: $OUTPUT_DIR/final.mp4"
    ls -lh "$OUTPUT_DIR/final.mp4"
}

cmd_workflow() {
    log "=== Full Kanaha Audio+Camera Workflow ==="
    echo ""

    cmd_status
    echo ""

    cmd_record
    echo ""

    cmd_analyze
    echo ""

    cmd_edl
    echo ""

    log "To assemble: edit $OUTPUT_DIR/edl.txt with video path, then run:"
    log "  $0 assemble --output $OUTPUT_DIR"
}

# --- Main ---

discover_devices
"cmd_${COMMAND}"
