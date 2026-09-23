/*
 * Kanaha Audio - on-device request resolver
 * Licensed under the Apache License, Version 2.0
 *
 * Turns one transcript of a spoken calculation request into one of four
 * outcomes, without a model and without the network:
 *
 *   KR_RUN     a complete spec, plus the read-back to say before running it
 *   KR_ASK     one slot is missing; `say` is the one question to ask
 *   KR_REFUSE  the request cannot be taken; `say` says why and what works
 *   KR_SILENT  nothing calculable was said (room chatter, a blank clip)
 *
 * It is a fixed grammar, not open language: the phrases rehearsed for the
 * demonstration and their known mishearings. Its rules are the orchestrator
 * prompt's rules made into code (kanaha-voice-orchestrator-prompt.md):
 *
 *  - Covariances are never accepted from speech.
 *  - A spoken number with more than two decimals is refused, not rounded.
 *  - A book shortens what a person says, never what is reported: the
 *    read-back expands it, every holding by ticker, before anything else.
 *  - Two candidate files are a question, never a silent pick.
 *  - The read-back names the operation as well as the data. A regime spoken
 *    without a verb is a variance, by rule, and the read-back says so; a
 *    simulation needs its own word ("simulate", "forward"). No verb is ever
 *    carried over from a previous turn.
 *
 * Pure C: no allocation, no I/O, no json-c. The caller supplies the books and
 * the file catalog (from kanaha-books.json and listCsvFiles) and a session
 * that remembers one pending question between turns.
 */

#ifndef KANAHA_RESOLVER_H
#define KANAHA_RESOLVER_H

#define KR_MAX_ASSETS   8
#define KR_MAX_BOOKS    4
#define KR_MAX_FILES    8
#define KR_MAX_SPOKEN   8
#define KR_MAX_COLUMNS  64
#define KR_TICKER_LEN   12
#define KR_NAME_LEN     64
#define KR_SAY_LEN      400

#define KR_DEFAULT_SEED        12345
#define KR_DEFAULT_SIMULATIONS 10000

/* A named portfolio, as kanaha-books.json defines it. */
typedef struct {
    char name[KR_NAME_LEN];                        /* "demo5" */
    char spoken[KR_MAX_SPOKEN][KR_NAME_LEN];       /* "the book", ... */
    int n_spoken;
    int n_assets;
    char tickers[KR_MAX_ASSETS][KR_TICKER_LEN];
    double weights_pct[KR_MAX_ASSETS];
    char file[KR_NAME_LEN];                        /* "fis_daily_closes.csv" */
    int window_years;                              /* 0 = the whole file */
} kr_book_t;

/* One CSV on the calcs phone, as listCsvFiles reports it: the tickers it
 * carries as <TICKER>_AdjClose. */
typedef struct {
    char name[KR_NAME_LEN];
    char tickers[KR_MAX_COLUMNS][KR_TICKER_LEN];
    int n_tickers;
} kr_file_t;

typedef struct {
    const kr_book_t *books;
    int n_books;
    const kr_file_t *files;
    int n_files;
} kr_context_t;

typedef enum { KR_SILENT, KR_ASK, KR_REFUSE, KR_RUN } kr_outcome_t;

typedef enum { KR_OP_NONE, KR_OP_VARIANCE, KR_OP_SIMULATE } kr_op_t;

typedef enum {
    KR_REGIME_HISTORICAL,   /* covarianceFromCsv on the file                 */
    KR_REGIME_STRESSED,     /* the file's vols, one spoken correlation       */
    KR_REGIME_HYPOTHETICAL  /* spoken vol and correlation, no file at all    */
} kr_regime_t;

typedef struct {
    kr_op_t op;
    kr_regime_t regime;
    const kr_book_t *book;          /* NULL when the names were spoken */
    int n_assets;
    char tickers[KR_MAX_ASSETS][KR_TICKER_LEN];
    double weights[KR_MAX_ASSETS];  /* fractions summing to 1 */
    int weights_equal;
    int weights_normalized;         /* spoken weights summed within 1 of 100 */
    const kr_file_t *file;          /* NULL for a hypothetical regime */
    int max_obs;                    /* 0 = the whole file */
    double rho;                     /* STRESSED and HYPOTHETICAL */
    double vol;                     /* HYPOTHETICAL: every asset's vol */
    int seed;                       /* simulate */
    int n_simulations;              /* simulate */

    /* As spoken, kept so a pending question can be finished next turn. */
    int have_rho;
    int n_weights_spoken;           /* 0 = equal */
    double weights_pct[KR_MAX_ASSETS];
    int window_years;               /* -1 = not said; 0 = the whole file */
    int window_months;              /* 0 = not said */
} kr_spec_t;

typedef enum {
    KR_SLOT_NONE, KR_SLOT_OPERATION, KR_SLOT_SUBJECT, KR_SLOT_FILE, KR_SLOT_CORRELATION
} kr_slot_t;

/* What survives between turns: at most one pending question. */
typedef struct {
    kr_slot_t missing;              /* KR_SLOT_NONE when nothing is pending */
    kr_spec_t spec;                 /* everything resolved so far */
    int have_subject;
} kr_session_t;

typedef struct {
    kr_outcome_t outcome;
    kr_spec_t spec;                 /* complete when outcome == KR_RUN */
    char say[KR_SAY_LEN];           /* read-back, question or refusal */
} kr_result_t;

void kr_session_init(kr_session_t *s);

void kr_resolve(const kr_context_t *ctx, kr_session_t *s,
                const char *transcript, kr_result_t *out);

/* The spoken name of a file: "the FIS file" for fis_daily_closes.csv. */
void kr_file_spoken(const kr_file_t *f, char *buf, int len);

#endif /* KANAHA_RESOLVER_H */
