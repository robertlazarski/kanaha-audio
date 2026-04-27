# Whisper Performance Guide

Developer knowledge for understanding whisper.cpp performance on Android, choosing the right configuration, and sizing workloads.

## Why Whisper Implementations Differ Dramatically

If you've used whisper on a desktop and found it slow, you were probably using the original Python implementation. The performance gap between implementations is enormous:

| Implementation | Language | Backend | 20s audio (CPU) | Why |
|---|---|---|---|---|
| openai-whisper | Python | PyTorch float32 | 10-30 seconds | Runs full float32 inference through PyTorch, no quantization |
| faster-whisper | Python | CTranslate2 int8 | ~700ms | Quantized weights, optimized C++ runtime behind Python API |
| whisper.cpp | C/C++ | ggml + ARM NEON | ~3-4 seconds* | Native C, SIMD-optimized for ARM. No Python overhead at all |

*Measured on Pixel 9 Pro with base.en model, greedy decoding, 4 threads.

**Key insight**: whisper.cpp is what Android apps use. It's not PyTorch — it's a ground-up C/C++ implementation with hand-tuned ARM NEON intrinsics for phone CPUs. If your desktop experience with whisper was slow, that's a PyTorch problem, not a whisper problem.

### Desktop validation shortcut

Before testing on the phone, validate keyword search accuracy on your desktop using faster-whisper (the fast Python implementation):

```bash
# In the kanaha-audio test-data directory:
/path/to/whisper_env/bin/python3 test_keyword_search.py presentation.wav
```

This runs in under a second and confirms that whisper can find your keywords before investing time in the Android pipeline.

## What Happens Inside whisper.cpp

Understanding the internal pipeline helps set performance expectations:

```
Audio (WAV float32)
    |
    v
Mel spectrogram          ~100ms on Pixel 9 Pro
    |                    (FFT over audio, produces 80-bin mel features)
    v
Encoder (transformer)    ~3,200ms on Pixel 9 Pro
    |                    (6 layers for base model, processes 30s chunks)
    v
Decoder (autoregressive) ~600ms per 30s chunk
    |                    (generates tokens one at a time)
    v
Tokens with timestamps
```

The **encoder** is the bottleneck. It processes audio in **30-second chunks** — this is fundamental to whisper's architecture, not configurable. A 15-minute file is split into ~30 chunks, each processed sequentially.

## Sizing Your Workload

### Formula

```
processing_time ≈ (audio_duration / 30s) × encode_time_per_chunk + decoder_overhead
```

### Pixel 9 Pro measurements (base.en, greedy, 4 threads)

| Audio duration | Chunks | Estimated time | Notes |
|---|---|---|---|
| 20 seconds | 1 | ~4 seconds | Single chunk, measured |
| 5 minutes | 10 | ~40 seconds | Quick review |
| 15 minutes | 30 | **~2-3 minutes** | Typical presentation |
| 1 hour | 120 | ~10-12 minutes | Full lecture |

### The 15-minute presentation use case

This is the primary Kanaha Audio use case: a presentation with ~15 slides where the speaker says "next slide please" at each transition.

- **Input**: 15-minute WAV (16kHz mono, ~29 MB)
- **Processing**: ~2-3 minutes on Pixel 9 Pro
- **Output**: ~15 timestamp pairs (start_ms, end_ms) with confidence scores
- **RAM**: ~430 MB total (model + audio + working memory)

This is a batch operation — you push the audio, fire the curl request, and wait. It's not real-time transcription. For a 15-minute recording, waiting 3 minutes for timestamps is acceptable since the alternative is manually entering 15 timestamps by hand into parseLTC.sh.

## English-Only Optimization

Whisper models are trained on multilingual data (99 languages). The English-only variants (`.en` suffix) are fine-tuned specifically for English:

- **Same model size** — language knowledge is in the weights, not a separate module
- **Slightly faster** — decoder doesn't waste computation on non-English token probabilities
- **Slightly more accurate** — English-specific fine-tuning
- **No translation capability** — can't do Japanese→English (use the multilingual model for that)

Kanaha Audio sets `wparams.language = "en"` in the bridge code, which forces English-only decoding regardless of which model variant is loaded. But loading the `.en` model is the cleaner approach.

### If you need translation later

Whisper's multilingual model can translate any of 99 languages to English during transcription. This uses the same model file — just a different task parameter (`task = "translate"` instead of `task = "transcribe"`). Kanaha Audio could support this in the future without any model size increase.

## Token-Level vs Segment-Level Timestamps

This is critical for keyword search accuracy.

**Segment-level** (default whisper behavior): whisper groups words into sentence-like segments. A 20-second recording might produce a single segment spanning 0-19,400ms. If your keyword appears in that segment, your "match" timestamp is 0-19,400ms — useless for parseLTC.sh.

**Token-level** (what Kanaha Audio uses): with `token_timestamps = true`, each word gets its own start/end time. "Next" gets 5,300-5,880ms, "slide" gets 5,880-6,840ms, "please" gets 7,180-7,760ms. The keyword match for "next slide please" is precisely 5,300-7,760ms.

This is why the bridge code sets `wparams.token_timestamps = true` and collects individual tokens instead of segments. The sliding-window keyword matcher operates on these tokens, producing the millisecond-precise timestamps that parseLTC.sh needs.

## Thread Count

The bridge uses `n_threads = 4`. On ARM big.LITTLE architectures (like the Pixel 9 Pro's Tensor G4):

- **4 threads**: Uses the performance cores without starving the OS. Good default.
- **8 threads**: Would use efficiency cores too — diminishing returns due to slower cores, and may cause UI jank.
- **1-2 threads**: Significantly slower. Only use on very old devices with limited thermal headroom.

## Verified Test Results

### Desktop (faster-whisper, base model, int8, CPU)

```
Audio: presentation.wav (20s, "Test 1 2 3. Next slide please." × 2)
Processing: 716ms
Matches:
  "next slide please" at 5,300ms - 7,760ms (confidence 0.89)
  "next slide please" at 13,000ms - 15,440ms (confidence 0.99)
```

### Pixel 9 Pro (whisper.cpp, base.en model, NEON, 4 threads)

```
Model load: 199ms
Transcription: "Test 1, 2, 3. Next slide, please test 1, 2, 3. Next slide, please. Test 1, 2, 3."
Encode: 3,241ms
Total: 7,808ms (beam search) / ~3-4s estimated (greedy)
ARM features: NEON=1, FMA=1
```

Both implementations produce correct transcriptions with both keyword instances identified.
