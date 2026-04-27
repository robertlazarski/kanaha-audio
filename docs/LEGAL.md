# Kanaha Audio - Legal Summary

## License

**Apache License 2.0**

Kanaha Audio is licensed under the Apache License 2.0. All dependencies use permissive or weak-copyleft licenses. One dependency (libltc) is LGPL-3.0; see the LGPL section below for compliance details.

## Third-Party Components

| Component | License | ASF Category | Usage |
|-----------|---------|--------------|-------|
| whisper.cpp | MIT | Category A | Speech-to-text inference engine |
| ggml | MIT | Category A | Tensor library for ML inference |
| TensorFlow Lite | Apache 2.0 | Same license | YAMNet audio event detection runtime |
| YAMNet model | Apache 2.0 | Same license | Pre-trained audio classifier (521 AudioSet classes) |
| Apache Axis2/C | Apache 2.0 | Same license | HTTP/2 JSON-RPC web services |
| Apache httpd | Apache 2.0 | Same license | HTTP/2 server |
| Apache APR | Apache 2.0 | Same license | Portable runtime |
| OpenSSL | Apache 2.0 | Same license | TLS/mTLS encryption |
| nghttp2 | MIT | Category A | HTTP/2 protocol |
| json-c | MIT | Category A | JSON parsing |
| Expat | MIT | Category A | XML parsing |
| libssh2 | BSD 3-Clause | Category A | SFTP file transfer |
| **libltc** | **LGPL-3.0** | **Category B** | SMPTE/LTC timecode decoding |
| AAudio | Android system lib | N/A | Microphone recording, speaker output |

## LGPL-3.0 Compliance (libltc)

libltc is the only weak-copyleft dependency. It is statically linked into the kanaha-audio-httpd binary. Per LGPL-3.0 §4, users must be able to relink the application with a modified version of libltc. Compliance is achieved by:

1. **Object files distributed in releases:** GitHub releases must include a `build-android-objects.tar.gz` containing all `.o` files from `build-android/`. This is the "Corresponding Application Code" required by LGPL-3.0 §4(d)(0)
2. **Build script:** `build-android.sh` contains the full link command, allowing relinking with a replacement `libltc.a`
3. **Source available:** libltc source is at https://github.com/x42/libltc (LGPL-3.0)

**Release checklist item:** Every release that includes the kanaha-audio-httpd binary must also include the application object files. The build script produces them in `build-android/`. Package them: `tar czf build-android-objects.tar.gz build-android/*.o` and attach to the GitHub release.

**Impact on upstream:** libltc is Category B under ASF policy. Code that directly calls libltc functions (specifically `kanaha-audio-app/app/src/main/cpp/ltc/ltc_decoder.c`) should be reviewed if contributing to an Apache project. The wrapper isolates libltc behind a clean API boundary — the rest of the codebase has no libltc dependency and remains Category A compatible.

## Comparison with Kanaha Camera

| Aspect | Kanaha Camera | Kanaha Audio |
|--------|---------------|---------------------|
| License | GPL v3+ (forced by OpenCamera) | Apache 2.0 |
| Can upstream to Apache? | No (ASF Category X) | Yes (with libltc isolation noted) |
| Corporate CLA compatible? | Requires legal review | Standard Apache CLA |
| Process separation needed? | Yes (GPL boundary via Intent IPC) | No (all permissive except libltc) |
| Copyleft dependencies | OpenCamera (GPL v3+) | libltc (LGPL-3.0, Category B) |

## Trademark Compliance

### Project Name: "Kanaha"

- **Origin:** Hawaiian geographic name (Kanaha Beach Park, Maui)
- **Status:** Geographic names are generally not trademarkable
- **Risk Level:** Very Low — comprehensive search found no conflicts in technology sector

### Third-Party Trademarks

The following are trademarks of their respective owners:

- **Apache, Apache Axis2/C, Apache HTTP Server** — The Apache Software Foundation
- **TensorFlow, TensorFlow Lite, YAMNet, Android, Pixel** — Google LLC
- **whisper.cpp, ggml** — ggml-org community projects
- **Tentacle Sync** — Tentacle Sync GmbH
- **Claude** — Anthropic

**Compliance:** Kanaha uses these names solely for technical attribution. The project is independent and not affiliated with, endorsed by, or sponsored by any trademark holders.

## Required Files

| File | Purpose |
|------|---------|
| `LICENSE` | Apache License 2.0 full text |
| `NOTICE` | Attribution for all third-party components (12 dependencies) |
| `TRADEMARKS.md` | Trademark acknowledgments and independence statement |

## Summary

Kanaha Audio is legally ready for public release:

- **License:** Apache 2.0 (11 permissive deps + 1 LGPL-3.0 dep with compliance documented)
- **Attribution:** NOTICE file credits all 12 components
- **LGPL compliance:** Object files available for relinking per LGPL-3.0 §4
- **Trademarks:** Proper acknowledgment, no false affiliation claims
- **Independence:** Clear statement of project independence
- **Upstream compatible:** Code can flow to Apache Axis2/C (libltc wrapper isolated in ltc_decoder.c)
