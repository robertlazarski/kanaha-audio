/*
 * Kanaha Audio - the voice chain from a laptop, without the microphone.
 * Licensed under the Apache License, Version 2.0
 *
 * Each line on stdin (or each argument) is one transcript. One session runs
 * across them, exactly as on the phone: resolve -> read-back -> the calls to
 * Kanaha Calcs over HTTP/2 + mTLS -> the answer, the SAY line and the trace.
 *
 *   kanaha_voice_cli --host 192.168.8.159 --port 8444 --name calcs.local \
 *       --ca ~/kanaha-ca/kanaha-ca.crt --cert ~/kanaha-ca/operator.crt \
 *       --key ~/kanaha-ca/operator.key --books kanaha-books.json \
 *       "portfolio variance on the book" "the book at correlation point eight"
 */

#include "../../kanaha-audio-app/app/src/main/cpp/voice/kanaha_calc.h"

#include <axutil_env.h>
#include <axutil_error_default.h>
#include <axutil_log_default.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *OUT[] = { "SILENT", "ASK", "REFUSE", "RUN" };

static void turn(const kr_context_t *ctx, kr_session_t *s, axis2_h2_json_client_t *c,
                 const axutil_env_t *env, const char *said)
{
    kr_result_t r;
    kc_result_t x;
    char answer[KA_ANSWER_LEN], say[KR_SAY_LEN], trace[KA_TRACE_LEN];

    kr_resolve(ctx, s, said, &r);
    printf("> %s\n  %s: %s\n", said, OUT[r.outcome], r.say);
    if (r.outcome != KR_RUN)
        return;
    kc_execute(c, env, &r.spec, &x);
    ka_answer(&r.spec, &x, answer, sizeof(answer), say, sizeof(say), trace, sizeof(trace));
    printf("  ANSWER: %s\n  SAY: %s\n", answer, say);
    if (trace[0])
        printf("  %s\n", trace);
    printf("  (%ld ms for %s)\n", x.round_trip_ms, x.tools[0] ? x.tools : "nothing");
}

int main(int argc, char **argv)
{
    axis2_h2_json_client_options_t o;
    const char *books_path = "kanaha-books.json";
    kr_book_t books[KR_MAX_BOOKS];
    kr_file_t files[KR_MAX_FILES];
    kr_context_t ctx;
    kr_session_t s;
    char err[256], line[2048];
    int i, first_said = argc;
    axutil_allocator_t *a = axutil_allocator_init(NULL);
    axutil_env_t *env = axutil_env_create_with_error_log(a, axutil_error_create(a),
                                                         axutil_log_create(a, NULL, "/dev/null"));
    axis2_h2_json_client_t *c;

    memset(&o, 0, sizeof(o));
    for (i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--host") == 0) o.host = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--port") == 0) o.port = atoi(argv[++i]);
        else if (i + 1 < argc && strcmp(argv[i], "--name") == 0) o.verify_name = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--ca") == 0) o.ca_file = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--cert") == 0) o.cert_file = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--key") == 0) o.key_file = argv[++i];
        else if (i + 1 < argc && strcmp(argv[i], "--books") == 0) books_path = argv[++i];
        else { first_said = i; break; }
    }

    ctx.n_books = kc_load_books(books_path, books, KR_MAX_BOOKS, err, sizeof(err));
    if (ctx.n_books < 0) { fprintf(stderr, "books: %s\n", err); return 1; }
    ctx.books = books;
    if (!(c = axis2_h2_json_client_create(env, &o))) {
        fprintf(stderr, "client: missing or invalid --host/--port/--ca/--cert/--key\n");
        return 1;
    }
    ctx.n_files = kc_fetch_catalog(c, env, files, KR_MAX_FILES, err, sizeof(err));
    if (ctx.n_files < 0) { fprintf(stderr, "catalog: %s\n", err); return 1; }
    ctx.files = files;
    printf("%d book(s), %d file(s) on the calcs phone\n", ctx.n_books, ctx.n_files);

    kr_session_init(&s);
    if (first_said < argc) {
        for (i = first_said; i < argc; i++) turn(&ctx, &s, c, env, argv[i]);
    } else {
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\r\n")] = '\0';
            turn(&ctx, &s, c, env, line);
        }
    }
    axis2_h2_json_client_free(c, env);
    axutil_env_free(env);
    return 0;
}
