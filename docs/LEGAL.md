# Kanaha Audio - Legal Summary

## License

**Apache License 2.0**

Kanaha Audio is licensed under the Apache License 2.0. All dependencies use permissive licenses (Apache 2.0 or MIT) — there is no copyleft (GPL) anywhere in the dependency tree.

## Third-Party Components

| Component | License | Category | Usage |
|-----------|---------|----------|-------|
| whisper.cpp | MIT | ASF Category A | Speech-to-text inference engine |
| ggml | MIT | ASF Category A | Tensor library for ML inference |
| Apache Axis2/C | Apache 2.0 | Same license | HTTP/2 JSON-RPC web services |
| Apache httpd | Apache 2.0 | Same license | HTTP/2 server |
| Apache APR | Apache 2.0 | Same license | Portable runtime |
| OpenSSL | Apache 2.0 | Same license | TLS/mTLS encryption |
| nghttp2 | MIT | ASF Category A | HTTP/2 protocol |
| json-c | MIT | ASF Category A | JSON parsing |
| Expat | MIT | ASF Category A | XML parsing |

**All dependencies are ASF Category A or same-license (Apache 2.0).** No Category B (weak copyleft) or Category X (strong copyleft) dependencies exist. Code from this project can flow upstream to Apache projects without license concerns.

## Comparison with Kanaha Camera

| Aspect | Kanaha Camera | Kanaha Audio |
|--------|---------------|---------------------|
| License | GPL v3+ (forced by OpenCamera) | Apache 2.0 |
| Can upstream to Apache? | No (ASF Category X) | Yes |
| Corporate CLA compatible? | Requires legal review | Standard Apache CLA |
| Process separation needed? | Yes (GPL boundary via Intent IPC) | No (all permissive) |
| Legal documents needed | 4 multi-page reviews | This single page |

## Trademark Compliance

### Project Name: "Kanaha"

- **Origin:** Hawaiian geographic name (Kanaha Beach Park, Maui)
- **Status:** Geographic names are generally not trademarkable
- **Risk Level:** Very Low — comprehensive search found no conflicts in technology sector

### Third-Party Trademarks

The following are trademarks of their respective owners:

- **Apache, Apache Axis2/C, Apache HTTP Server** — The Apache Software Foundation
- **whisper.cpp, ggml** — ggml-org community projects

**Compliance:** Kanaha uses these names solely for technical attribution. The project is independent and not affiliated with, endorsed by, or sponsored by any trademark holders.

## Required Files

| File | Purpose |
|------|---------|
| `LICENSE` | Apache License 2.0 full text |
| `NOTICE` | Attribution for all third-party components |
| `TRADEMARKS.md` | Trademark acknowledgments and independence statement |

## Summary

Kanaha Audio is legally ready for public release:

- **License:** Apache 2.0 (all dependencies permissive)
- **Attribution:** NOTICE file credits all components
- **Trademarks:** Proper acknowledgment, no false affiliation claims
- **Independence:** Clear statement of project independence
- **Upstream compatible:** Code can flow to Apache Axis2/C directly
