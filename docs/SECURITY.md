# Security Model

Kanaha Audio shares the same security model as Kanaha Camera Control System.

## Transport Security — TLS is Mandatory

Axis2/C in JSON-RPC mode uses HTTP/2, which requires TLS. The server **will not start** without certificate and key paths. There is no plain HTTP fallback.

- **HTTPS/HTTP2 + mTLS**: All API communication uses TLS with mutual certificate authentication
- **Certificate chain**: Self-signed CA → server cert + client cert
- **No anonymous access**: Connections without valid client certificates are rejected at the TLS handshake
- **No plain HTTP**: Server refuses to start without `-c <cert> -k <key>` arguments
- **Minimum TLS 1.2**: Enforced via `SSL_CTX_set_min_proto_version(TLS1_2_VERSION)`

### Certificate Configuration

```bash
# Server requires these flags to start:
kanaha-audio-httpd -p 8443 \
  -c /path/to/server.crt \    # Server certificate (PEM)
  -k /path/to/server.key \    # Server private key (PEM)
  -a /path/to/ca.crt           # CA cert for client verification (mTLS)
```

Without `-a` (CA cert), the server runs TLS but does not verify client certificates. For production, always provide the CA cert to enable mTLS.

Uses the same certificate infrastructure as Kanaha Camera. See the Kanaha Camera documentation for certificate generation and deployment instructions.

## Input Validation

- Audio file paths are validated against path traversal attacks (`..` sequences rejected)
- Model names are restricted to plain names (no `/` or `..` — e.g., "base", "tiny")
- Keyword strings are sanitized before use in whisper.cpp
- JSON string parsing handles escaped quotes to prevent injection
- HTTP method routing uses `strncmp` anchored at position 0 (prevents request smuggling)
- The `listAudioFiles` directory parameter falls back to the default app directory if `..` is detected

## Architecture Security Advantage

Unlike Kanaha Camera (which requires Intent IPC across a GPL process boundary), Kanaha Audio calls whisper.cpp directly as a C function call within the same process. This eliminates:

- IPC serialization/deserialization attack surface
- File-based response passing (no TOCTOU race conditions)
- Shell invocation risks (no `system()`, `popen()`, or `am broadcast` subprocess)

## Reporting Vulnerabilities

Report security issues via GitHub Issues with the "security" label.
