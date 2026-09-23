# EDL Generation — Kanaha Audio

## What It Does

Kanaha Audio can automatically generate an Edit Decision List (EDL) from audio cues detected in a room recording. The EDL tells ffmpeg how to cut and assemble a video — which segments to keep, where to insert slide images, and when to start and stop.

This replaces manually typing timecodes into a script. Instead of a human watching footage and writing down timestamps, the phone listens to the room and figures out the edit points.

## The Problem

In `parseLTC.sh`, lines 79-98 are magic values that a human must set by hand before every session:

```bash
# EDIT STARTS HERE
cameraTransitionTimeDivisionInSeconds=20
timecode_date=2020-01-30
epoch_secs_start_time=$(date +%s --date="$timecode_date 00:08:00")
epoch_secs_stop_time=$(date +%s --date="$timecode_date 00:13:12")
dropFrames=1
hasOptionalImageAppend=0
hasOptionalFirstCutsHalfDuration=1
hasOptionalAudioFadeInAndOut=0
# EDIT ENDS HERE
```

These values change every session. Get them wrong and the video is broken.

## The Solution

Replace magic values with audio detection. A second phone (e.g., a Moto G Play 2024, about $99) sits in the room recording audio. Kanaha Audio analyzes that recording and finds the edit points automatically:

- **"When does the show start?"** → YAMNet detects music (e.g., saxophone starts playing)
- **"When to cut to a slide?"** → whisper.cpp detects "next slide please"
- **"When does the show end?"** → YAMNet detects the last music (e.g., drum solo ends)

## How It Works

### Step 1: Record Room Audio

A phone running Kanaha Audio records the room with its built-in microphone:

```bash
# Start recording on the room-mic phone
curl -sk --http2 \
  --cert client.crt --key client.key --cacert ca.crt \
  -H "Content-Type: application/json" \
  -d '{"action":"startRecording","clip_name":"room","sample_rate":16000}' \
  "https://$MOTO:8443/services/AudioSearchService/startRecording"
```

### Step 2: Detect Audio Cues

After recording, ask the phone to analyze the audio:

```bash
# Find music boundaries (start and end of the performance)
curl ... -d '{"action":"detectAudioEvents",
  "audio_file":"/path/to/room.wav",
  "events":["Music"]}' \
  "https://$MOTO:8443/services/AudioSearchService/detectAudioEvents"
```

Response:
```json
{
  "detections": [
    {"event": "Music", "start_ms": 46100, "end_ms": 47075, "confidence": 0.94},
    {"event": "Music", "start_ms": 103200, "end_ms": 104175, "confidence": 0.55}
  ]
}
```

Music starts at **46.1 seconds**, last music at **103.2 seconds**.

```bash
# Find "next slide please" keywords
curl ... -d '{"action":"searchKeywords",
  "audio_file":"/path/to/room.wav",
  "keywords":["next slide please"]}' \
  "https://$MOTO:8443/services/AudioSearchService/searchKeywords"
```

Response:
```json
{
  "matches": [
    {"keyword": "next slide please", "start_ms": 62080, "confidence": 0.98},
    {"keyword": "next slide please", "start_ms": 73320, "confidence": 1.0}
  ]
}
```

Keywords at **62.1 seconds** and **73.3 seconds**.

### Step 3: Generate EDL

This is where the cue timestamps become an edit list. The logic is simple and runs entirely in bash:

```bash
#!/bin/bash
# generate-edl-simple.sh — Pure bash EDL generation from audio cue JSON

VIDEO="my_video.mp4"
SLIDE="slide_3s.mp4"

# Read cue times from the JSON files produced in Step 2
# (jq extracts values from JSON — install with: apt install jq)
MUSIC_START=$(jq '.detections[0].start_ms / 1000' events.json)
MUSIC_END=$(jq '.detections[-1].end_ms / 1000' events.json)

# Get keyword timestamps, skip duplicates within 8 seconds of each other
KEYWORDS=()
PREV=0
for ms in $(jq -r '.matches[].start_ms' keywords.json); do
    secs=$((ms / 1000))
    if (( secs - PREV > 8 )); then
        KEYWORDS+=("$secs")
    fi
    PREV=$secs
done

echo "Music:    ${MUSIC_START}s → ${MUSIC_END}s"
echo "Keywords: ${KEYWORDS[*]}"

# Format seconds as HH:MM:SS.mmm for ffmpeg
fmt() {
    local s=$1
    local h=$((s / 3600))
    local m=$(( (s % 3600) / 60 ))
    local sec=$(echo "$s" | awk "{printf \"%.3f\", \$1 % 60}")
    printf "%02d:%02d:%s" "$h" "$m" "$sec"
}

# Build the EDL file
# The EDL alternates between video segments and slide inserts:
#
#   [Video: music start → first keyword]
#   [Slide: 3 seconds]
#   [Video: after first slide → second keyword]
#   [Slide: 3 seconds]
#   [Video: after second slide → music end]

SLIDE_DURATION=3
EDL_FILE="edl.txt"
> "$EDL_FILE"

CURSOR="$MUSIC_START"

for kw_time in "${KEYWORDS[@]}"; do
    # Video segment: from cursor to this keyword
    if (( $(echo "$kw_time > $CURSOR" | bc -l) )); then
        echo "file '$VIDEO'"       >> "$EDL_FILE"
        echo "inpoint $(fmt $CURSOR)"  >> "$EDL_FILE"
        echo "outpoint $(fmt $kw_time)" >> "$EDL_FILE"
    fi

    # Slide insert
    echo "file '$SLIDE'" >> "$EDL_FILE"

    # Advance cursor past the slide duration
    CURSOR=$(echo "$kw_time + $SLIDE_DURATION" | bc)
done

# Final video segment: from after last slide to music end
if (( $(echo "$MUSIC_END > $CURSOR" | bc -l) )); then
    echo "file '$VIDEO'"        >> "$EDL_FILE"
    echo "inpoint $(fmt $CURSOR)"   >> "$EDL_FILE"
    echo "outpoint $(fmt $MUSIC_END)" >> "$EDL_FILE"
fi

echo ""
echo "=== Generated EDL ==="
cat "$EDL_FILE"
```

The output `edl.txt` looks like this:

```
file 'my_video.mp4'
inpoint 00:00:46.100
outpoint 00:01:02.000
file 'slide_3s.mp4'
file 'my_video.mp4'
inpoint 00:01:05.000
outpoint 00:01:13.000
file 'slide_3s.mp4'
file 'my_video.mp4'
inpoint 00:01:16.000
outpoint 00:01:43.200
```

This is the [ffmpeg concat demuxer format](https://ffmpeg.org/ffmpeg-formats.html#concat-1). Each `file` / `inpoint` / `outpoint` block is one segment. ffmpeg reads this file and splices the segments together.

### Step 4: Assemble the Video

One ffmpeg command produces the final video with audio fade in/out:

```bash
# Assemble with 2-second fade in, 3-second fade out
ffmpeg -y -f concat -safe 0 -i edl.txt \
  -c:v libx264 -crf 23 \
  -c:a aac -b:a 128k \
  -af "afade=t=in:st=0:d=2,afade=t=out:st=54:d=3" \
  -movflags +faststart \
  final.mp4
```

### Step 5 (Optional): Create the Slide Video

The slide is a short video made from a JPEG image with a silent audio track:

```bash
ffmpeg -y -loop 1 -i slide.jpg \
  -f lavfi -i anullsrc=r=48000:cl=mono \
  -c:v libx264 -crf 23 -pix_fmt yuv420p -r 30 \
  -c:a aac -b:a 128k \
  -t 3 -shortest \
  slide_3s.mp4
```

The silent audio track is required — without it, ffmpeg concat produces broken output when mixing video-with-audio and video-without-audio segments.

## Complete Workflow Example

```bash
#!/bin/bash
# Full example: record → analyze → generate EDL → assemble video

MOTO=192.168.8.126    # room-mic phone running Kanaha Audio (a Moto G Play 2024 here)
PIXEL=192.168.8.159   # Pixel 10 Pro XL running Kanaha Camera (video)
SSL=~/kanaha-certs   # your CA dir: client.crt, client.key, ca.crt
CURL="curl -sk --http2 --cert $SSL/client.crt --key $SSL/client.key --cacert $SSL/ca.crt -H Content-Type:application/json"

# 1. Start both devices
$CURL -d '{"action":"startRecording","clip_name":"room","sample_rate":16000}' \
  "https://$MOTO:8443/services/AudioSearchService/startRecording"
$CURL -d '{"action":"startRecording"}' \
  "https://$PIXEL:8443/services/CameraControlService/startRecording"

echo "Recording... press Enter when done"
read

# 2. Stop both
ROOM=$($CURL -d '{"action":"stopRecording"}' \
  "https://$MOTO:8443/services/AudioSearchService/stopRecording")
$CURL -d '{"action":"stopRecording"}' \
  "https://$PIXEL:8443/services/CameraControlService/stopRecording"

echo "Room recording: $ROOM"

# 3. Load whisper and analyze
$CURL -d '{"action":"loadModel","model":"base.en"}' \
  "https://$MOTO:8443/services/AudioSearchService/loadModel"

AUDIO_FILE="/data/user/0/org.kanaha.audio/files/audio/room.wav"

$CURL -d "{\"action\":\"detectAudioEvents\",\"audio_file\":\"$AUDIO_FILE\",\"events\":[\"Music\"]}" \
  "https://$MOTO:8443/services/AudioSearchService/detectAudioEvents" > events.json

$CURL -d "{\"action\":\"searchKeywords\",\"audio_file\":\"$AUDIO_FILE\",\"keywords\":[\"next slide please\"]}" \
  "https://$MOTO:8443/services/AudioSearchService/searchKeywords" > keywords.json

# 4. Pull video, generate EDL, assemble
adb pull /storage/emulated/0/DCIM/OpenCamera/*.mp4 video.mp4
./generate-edl-simple.sh    # produces edl.txt
ffmpeg -y -f concat -safe 0 -i edl.txt \
  -c:v libx264 -crf 23 -c:a aac -b:a 128k \
  -af "afade=t=in:st=0:d=2,afade=t=out:st=54:d=3" \
  final.mp4
```

## What Replaces What

| parseLTC.sh (manual) | Kanaha Audio EDL (automatic) |
|----------------------|------------------------------|
| `epoch_secs_start_time` (line 84) | YAMNet `detectAudioEvents` → first Music detection |
| `epoch_secs_stop_time` (line 86) | YAMNet `detectAudioEvents` → last Music detection |
| `epoch_secs_third_camera_exclusive` (lines 119-120) | whisper `searchKeywords` → "next slide please" timestamps |
| `magicNumberOffset=0.017` (line 192) | Not needed — `decodeLTC` returns sample-accurate positions |
| `cameraTransitionTimeDivisionInSeconds=20` (line 80) | Still configurable, or driven by keyword triggers |
| `hasOptionalImageAppend` (line 91) | Slide insert via keyword detection |
| `hasOptionalAudioFadeInAndOut` (line 97) | ffmpeg `-af afade` in the assemble step |
| `q81_out.csv` / `q82_out.csv` (generated) | `edl.txt` (ffmpeg concat demuxer format) |
| `cuts.txt` (generated) | Same `edl.txt` — one file replaces both CSV + cuts.txt |

## Tools Used

| Tool | What It Does | Installed On |
|------|-------------|-------------|
| `detectAudioEvents` | Finds music, speech, instruments (YAMNet) | Phone (Kanaha Audio) |
| `searchKeywords` | Finds spoken phrases with timestamps (whisper.cpp) | Phone (Kanaha Audio) |
| `decodeLTC` | Decodes SMPTE timecode from Tentacle Sync recording (libltc) | Phone (Kanaha Audio) |
| `jq` | Extracts values from JSON in bash | Laptop (`apt install jq`) |
| `ffmpeg` | Creates slide video, assembles final video | Laptop |
| `bc` | Floating-point arithmetic in bash | Laptop (usually pre-installed) |

## Related

- [test-audio-workflow.sh](../tools/test-audio-workflow.sh) — Full workflow script with device discovery
- [generate-edl.sh](../tools/generate-edl.sh) — EDL generation with LTC + audio cues
- [MCP.md](MCP.md) — MCP tools for Claude Desktop integration
