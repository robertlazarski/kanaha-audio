/*
 * Kanaha Audio - the voice request path inside the audio service
 * Licensed under the Apache License, Version 2.0
 *
 * One entry point that takes a transcript and does everything the laptop
 * orchestrator used to: resolve it, call Kanaha Calcs on the other phone over
 * HTTP/2 + mTLS with this phone's own certificate, word the answer, and
 * (optionally) say it on this phone's speaker.
 *
 * Configuration lives under the app's files directory:
 *
 *   voice/voice.json          {"calcs": {"host": "192.168.8.159", "port": 8444,
 *                                        "verify_name": "calcs.local"}}
 *   voice/kanaha-books.json   the books, same file the orchestrator reads
 *   apache/ssl/server.crt     this phone's certificate (clientAuth), its key,
 *   apache/ssl/server.key     and the CA the calcs phone's certificate
 *   apache/ssl/ca.crt         chains to -- all placed by provisioning
 *
 * The addresses are static; finding them is a laptop prep step (mDNS). One
 * session, one connection and one catalog per process, behind one mutex:
 * requests are handled one at a time, in the order they arrive.
 */

#ifndef KANAHA_VOICE_H
#define KANAHA_VOICE_H

#include <stddef.h>

/* Remember where the files directory is. Nothing is read until a request. */
void kanaha_voice_init(const char *files_dir);

/* Handle one transcript. Writes a JSON object into json_out:
 *   {"success":true,"outcome":"RUN|ASK|REFUSE|SILENT","readback":"...",
 *    "answer":"...","say":"...","trace":"SPEC | ...","round_trip_ms":N}
 * When speak is set, says the read-back or question, then the SAY line.
 * Returns 0, or -1 with {"success":false,"error":"..."} in json_out. */
int kanaha_voice_request(const char *transcript, int speak, char *json_out, size_t out_size);

/* Forget the pending question, the cached catalog and the connection, so the
 * next request rereads voice.json and the books (a new speaker, a new calcs
 * address, or the calcs phone's files changed). */
void kanaha_voice_reset(void);

#endif /* KANAHA_VOICE_H */
