# MCP (Model Context Protocol) — Kanaha Audio

## Overview

Kanaha Audio exposes all audio operations as MCP tools, enabling Claude Desktop and other MCP clients to record audio, run speech-to-text, detect instruments, decode SMPTE timecode, and transfer files — directly from an AI conversation.

The MCP binary reads JSON-RPC 2.0 from stdin and writes responses to stdout. It shares the same service implementation as the HTTP/2 API, so every operation available via curl is also available via MCP.

## Architecture

```
Claude Desktop (or any MCP client)
    │
    ├─ JSON-RPC 2.0 over stdio
    │
    ▼
kanaha-audio-mcp (98 KB native binary)
    │
    ├─ initialize    → protocol handshake
    ├─ tools/list    → 14 audio tool catalog
    └─ tools/call    → dispatches to audio_search_service_invoke_json_impl()
                          │
                          ├─ whisper.cpp    (speech-to-text, keyword search)
                          ├─ YAMNet/TFLite  (audio event detection)
                          ├─ AAudio NDK     (microphone recording, tone playback)
                          ├─ libltc         (SMPTE/LTC timecode decoding)
                          └─ libssh2        (SFTP file transfer)
```

No network connection needed — MCP runs on the device itself.

## Tools

| Tool | Description |
|------|-------------|
| `searchKeywords` | Find keyword phrases with timestamps (whisper.cpp) |
| `transcribe` | Full transcription with word-level timestamps |
| `detectAudioEvents` | Detect instruments, speech, music (YAMNet, 521 AudioSet classes) |
| `startRecording` | Record from microphone to WAV (optional `start_at` scheduling) |
| `stopRecording` | Stop recording, finalize WAV |
| `playTone` | Sine wave through speaker (sync slate, `start_at` support) |
| `listRecordings` | List WAV recordings with sizes and durations |
| `getStatus` | Model state, recording state, device info |
| `listModels` | Available whisper models on device |
| `loadModel` | Load/switch whisper model |
| `listAudioFiles` | List processable audio files |
| `decodeLTC` | Decode SMPTE/LTC timecode from WAV (libltc) |
| `sftpTransfer` | Transfer audio files to storage server via SFTP |

## Claude Desktop Configuration

Add to `~/.config/claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "kanaha-audio": {
      "command": "adb",
      "args": [
        "shell",
        "/data/data/org.kanaha.audio/files/kanaha-audio-mcp"
      ]
    }
  }
}
```

For a specific device (when multiple phones are connected):

```json
{
  "mcpServers": {
    "kanaha-audio-moto": {
      "command": "adb",
      "args": [
        "-s", "ZY224HBVQF",
        "shell",
        "/data/data/org.kanaha.audio/files/kanaha-audio-mcp"
      ]
    }
  }
}
```

## Protocol

MCP version: `2024-11-05`
Transport: JSON-RPC 2.0 over stdio (newline-delimited)

### Handshake

```
→ {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}}
← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"kanaha-audio","version":"1.0.0"}}}
```

### Tool Discovery

```
→ {"jsonrpc":"2.0","id":2,"method":"tools/list"}
← {"jsonrpc":"2.0","id":2,"result":{"tools":[...]}}
```

### Tool Call

```
→ {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"searchKeywords","arguments":{"audio_file":"/path/to/recording.wav","keywords":["next slide please"]}}}
← {"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"{\"success\":true,\"matches\":[...]}"}]}}
```

## Example Session

### Record and search for keywords

```bash
# List available tools
echo '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | \
  adb shell /data/data/org.kanaha.audio/files/kanaha-audio-mcp

# Load whisper model
echo '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"loadModel","arguments":{"model":"base.en"}}}' | \
  adb shell /data/data/org.kanaha.audio/files/kanaha-audio-mcp

# Start recording
echo '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"startRecording","arguments":{"clip_name":"meeting","sample_rate":16000}}}' | \
  adb shell /data/data/org.kanaha.audio/files/kanaha-audio-mcp

# ... wait ...

# Stop recording
echo '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"stopRecording","arguments":{}}}' | \
  adb shell /data/data/org.kanaha.audio/files/kanaha-audio-mcp

# Search for keywords
echo '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"searchKeywords","arguments":{"audio_file":"/data/data/org.kanaha.audio/files/audio/meeting.wav","keywords":["next slide please"]}}}' | \
  adb shell /data/data/org.kanaha.audio/files/kanaha-audio-mcp
```

### Decode SMPTE timecode

```bash
echo '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"decodeLTC","arguments":{"audio_file":"/data/data/org.kanaha.audio/files/audio/tentacle_smpte.wav"}}}' | \
  adb shell /data/data/org.kanaha.audio/files/kanaha-audio-mcp
```

### Detect instruments

```bash
echo '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"detectAudioEvents","arguments":{"audio_file":"/data/data/org.kanaha.audio/files/audio/room.wav","events":["Saxophone","Drum","Speech","Music"]}}}' | \
  adb shell /data/data/org.kanaha.audio/files/kanaha-audio-mcp
```

## Multi-Device MCP

Claude Desktop can connect to multiple Kanaha devices simultaneously:

```json
{
  "mcpServers": {
    "kanaha-camera": {
      "command": "adb",
      "args": ["-s", "PIXEL_SERIAL", "shell", "/data/data/org.kanaha.camera/files/kanaha-mcp"]
    },
    "kanaha-audio-room": {
      "command": "adb",
      "args": ["-s", "MOTO_SERIAL", "shell", "/data/data/org.kanaha.audio/files/kanaha-audio-mcp"]
    }
  }
}
```

This gives Claude access to both camera control (9 tools) and audio analysis (14 tools) across devices. Example Claude conversation:

> "Record 30 seconds of room audio on the Moto, then search for 'next slide please' and tell me the timestamps"

Claude would call `startRecording` → wait → `stopRecording` → `searchKeywords`, all through MCP.

## Comparison with Kanaha Camera MCP

| Aspect | Kanaha Camera | Kanaha Audio |
|--------|--------------|--------------|
| License | GPL-3.0-or-later | Apache-2.0 |
| Tools | 9 (camera control) | 13 (audio + ML + LTC + SFTP) |
| ML Models | None | whisper.cpp + YAMNet |
| Binary | kanaha-mcp (98 KB) | kanaha-audio-mcp |
| Protocol | MCP 2024-11-05 | MCP 2024-11-05 |
| Transport | stdio (JSON-RPC 2.0) | stdio (JSON-RPC 2.0) |

Both implementations follow the same pattern but are independently written. The camera MCP is GPL (due to OpenCamera dependency); the audio MCP is Apache 2.0 (all dependencies are permissive).

## Network Discovery

Kanaha Audio registers as an mDNS service for zero-config network discovery:

```bash
# Discover audio devices on the network
avahi-browse -rp _https._tcp | grep "kanaha-audio"

# Or use the discovery script
./tools/kanaha-audio-discover.sh
./tools/kanaha-audio-discover.sh --json
```

The mDNS TXT record includes `api=kanaha-audio` to distinguish from Kanaha Camera devices (`api=kanaha-camera-control`).

## Related Documentation

- [Kanaha Camera MCP](https://github.com/robertlazarski/kanaha/blob/main/docs/MCP.md) — Camera control MCP tools
- [SFTP File Transfer](docs/SFTP-FILE-TRANSFER.md) — Secure file transfer setup
- [SECURITY.md](docs/SECURITY.md) — mTLS security model
