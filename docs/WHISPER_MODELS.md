# Whisper Models for Android

## Multilingual vs English-Only

Every model tier comes in two variants:

| Variant | File | Languages | English accuracy | Use case |
|---------|------|-----------|-----------------|----------|
| Multilingual | `ggml-base.bin` | 99 languages + translation | Good | Translation (e.g., Japanese → English) |
| English-only | `ggml-base.en.bin` | English only | Slightly better | Keyword search, English transcription |

The file sizes are identical — language support is baked into the training weights, not a separate module. The English-only variant is marginally faster because the decoder doesn't allocate probability to non-English tokens.

**For Kanaha Audio keyword search, use the English-only variant** (`.en`). The bridge code sets `language = "en"` which forces English decoding on either variant, but the `.en` model is the cleaner choice.

## Model Tiers

Measured on Pixel 9 Pro (Tensor G4, 4 threads, greedy decoding):

| Model | File size | RAM usage | 20s audio | 15 min audio | Recommended for |
|-------|-----------|-----------|-----------|--------------|-----------------|
| tiny.en | 75 MB | ~390 MB | ~1.5s | ~1 min | Development, quick tests |
| base.en | 142 MB | ~400 MB | ~3-4s | ~3 min | **Keyword search (default)** |
| small.en | 466 MB | ~1 GB | ~15s | ~12 min | Higher accuracy needs |
| medium.en | 1.5 GB | ~2.6 GB | ~60s | ~45 min | Maximum accuracy |

Actual Pixel 9 Pro measurement with `base.en` model, 20-second test file:
- Model load: 199ms
- Encode (inference): 3,241ms
- Total (with beam search): 7,808ms
- Estimated greedy mode: ~3-4 seconds

## Model Files

Models in ggml format go in the app's models directory:

```
/data/data/org.kanaha.audio/files/models/
    ggml-base.en.bin      <- recommended default
    ggml-tiny.en.bin      <- fast development/testing
```

Download from the whisper.cpp repo:
```bash
cd whisper.cpp/models
bash download-ggml-model.sh base.en
```

Or push directly to the phone:
```bash
adb push ggml-base.en.bin /data/local/tmp/
```

## Audio File Requirements

- **Format**: WAV, PCM 16-bit signed integer
- **Ideal**: 16kHz sample rate, mono channel
- **Conversion**: `ffmpeg -i input.mp4 -ar 16000 -ac 1 -c:a pcm_s16le output.wav`
- **Multi-channel**: The bridge averages channels to mono automatically
- **Other sample rates**: Accepted but 16kHz gives best results

## Memory Budget

The model + working memory + audio buffer must fit in available RAM:

| Component | base.en | small.en |
|-----------|---------|----------|
| Model weights | 147 MB | 466 MB |
| KV cache + compute buffers | ~250 MB | ~600 MB |
| 15 min audio (float32) | 29 MB | 29 MB |
| **Total** | **~430 MB** | **~1.1 GB** |

Pixel 9 Pro (16 GB): any model works. Older phones (4-6 GB): stick with tiny or base.
