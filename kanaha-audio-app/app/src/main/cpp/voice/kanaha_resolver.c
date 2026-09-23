/*
 * Kanaha Audio - on-device request resolver
 * Licensed under the Apache License, Version 2.0
 *
 * See kanaha_resolver.h for the contract. The work is in three steps:
 *
 *   1. normalise + tokenise the transcript (words, numbers, percent signs),
 *   2. find the features: operation, subject (book or names), regime
 *      (correlation, vol), weights, window, file, and the refusal triggers,
 *   3. complete the spec -- or stop at the first missing slot and ask for it.
 *
 * Every refusal and question is a sentence someone reads aloud, so each one
 * says what would work instead.
 */

#include "kanaha_resolver.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 160
#define TOK_LEN    32
#define DAYS_PER_YEAR   252
#define DAYS_PER_MONTH  21
#define MAX_OBS         5000

/* ------------------------------------------------------------------------ */
/* tokens                                                                    */
/* ------------------------------------------------------------------------ */

typedef enum { T_WORD, T_NUM, T_PCT } tok_type_t;

typedef struct {
    tok_type_t type;
    char text[TOK_LEN];
    double value;       /* T_NUM */
    int decimals;       /* T_NUM: digits after the point */
} tok_t;

typedef struct {
    tok_t t[MAX_TOKENS];
    int n;
} toks_t;

static void push(toks_t *ts, tok_type_t type, const char *s, int len)
{
    tok_t *k;
    if (ts->n >= MAX_TOKENS || len <= 0)
        return;
    if (len >= TOK_LEN)
        len = TOK_LEN - 1;
    k = &ts->t[ts->n++];
    k->type = type;
    memcpy(k->text, s, (size_t)len);
    k->text[len] = '\0';
    k->value = 0;
    k->decimals = 0;
    if (type == T_NUM) {
        const char *dot = strchr(k->text, '.');
        k->value = strtod(k->text, NULL);
        k->decimals = dot ? (int)strlen(dot + 1) : 0;
    }
}

/*
 * Lowercase, drop whisper's bracketed tags ("[BLANK_AUDIO]", "[BEEP]") and
 * speaker marks (">>"), and split into words, numbers and '%'.
 *
 *  - A period after a single letter is an initial: "J.P. Morgan" becomes
 *    "jp morgan", "F.I.S." becomes "fis". Splitting there would lose JPM.
 *  - A period before a digit starts a number: "correlation.8" is
 *    "correlation" then ".8".
 *  - '&' is "and": "Johnson & Johnson" and "Johnson and Johnson" are the same
 *    company and whisper picks either spelling.
 *  - A '-' directly before a digit is a sign; anywhere else it separates
 *    ("twenty-two", "co-variance").
 */
static void tokenise(const char *in, toks_t *ts)
{
    char buf[1024];
    int n = 0, i;
    size_t len = strlen(in);

    ts->n = 0;
    for (i = 0; i < (int)len && n < (int)sizeof(buf) - 8; i++) {
        char c = in[i];
        if (c == '[') {
            const char *close = strchr(in + i, ']');
            if (close) { i = (int)(close - in); buf[n++] = ' '; continue; }
        }
        if (c == '>' && in[i + 1] == '>') { i++; buf[n++] = ' '; continue; }
        if (c == '&') { memcpy(buf + n, " and ", 5); n += 5; continue; }
        if (c == '.') {
            int single = i > 0 && isalpha((unsigned char)in[i - 1]) &&
                         (i < 2 || !isalpha((unsigned char)in[i - 2]));
            if (single)
                continue;                               /* an initial */
            if (isdigit((unsigned char)in[i + 1])) {
                if (i == 0 || !isdigit((unsigned char)in[i - 1]))
                    buf[n++] = ' ';                     /* ".8" starts a number */
                buf[n++] = '.';
                continue;
            }
            buf[n++] = ' ';
            continue;
        }
        if (c == '-' && (isdigit((unsigned char)in[i + 1]) ||
                         (in[i + 1] == '.' && isdigit((unsigned char)in[i + 2])))) {
            buf[n++] = ' ';
            buf[n++] = '-';
            continue;
        }
        if (c == '%') { buf[n++] = ' '; buf[n++] = '%'; buf[n++] = ' '; continue; }
        if (c == '\'')
            continue;                                   /* "it's" -> "its" */
        if (isalpha((unsigned char)c)) { buf[n++] = (char)tolower((unsigned char)c); continue; }
        if (isdigit((unsigned char)c)) {
            if (n > 0 && isalpha((unsigned char)buf[n - 1]))
                buf[n++] = ' ';                         /* "10yr" -> "10 yr" */
            buf[n++] = c;
            continue;
        }
        buf[n++] = ' ';
    }
    buf[n] = '\0';

    for (i = 0; i < n;) {
        int j = i;
        if (buf[i] == ' ') { i++; continue; }
        if (buf[i] == '%') { push(ts, T_PCT, "%", 1); i++; continue; }
        if (isalpha((unsigned char)buf[i])) {
            while (j < n && isalpha((unsigned char)buf[j])) j++;
            push(ts, T_WORD, buf + i, j - i);
            if (j < n && isdigit((unsigned char)buf[j])) { i = j; continue; }
            i = j;
            continue;
        }
        if (buf[i] == '-' || buf[i] == '.' || isdigit((unsigned char)buf[i])) {
            int seen_dot = 0;
            if (buf[j] == '-') j++;
            while (j < n && (isdigit((unsigned char)buf[j]) || (buf[j] == '.' && !seen_dot))) {
                if (buf[j] == '.') seen_dot = 1;
                j++;
            }
            if (j > i && buf[j - 1] == '.') j--;         /* "22." -> "22" */
            if (j == i || (j == i + 1 && (buf[i] == '-' || buf[i] == '.'))) { i++; continue; }
            push(ts, T_NUM, buf + i, j - i);
            i = j;
            continue;
        }
        i++;
    }
}

static int is_w(const toks_t *ts, int i, const char *words)
{
    const char *p = words;
    size_t wl;
    if (i < 0 || i >= ts->n || ts->t[i].type != T_WORD)
        return 0;
    wl = strlen(ts->t[i].text);
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t l = bar ? (size_t)(bar - p) : strlen(p);
        if (l == wl && strncmp(p, ts->t[i].text, l) == 0)
            return 1;
        if (!bar) break;
        p = bar + 1;
    }
    return 0;
}

static int find_w(const toks_t *ts, const char *words)
{
    int i;
    for (i = 0; i < ts->n; i++)
        if (is_w(ts, i, words))
            return i;
    return -1;
}

/* Does the token sequence at i spell `phrase` (space-separated words)? */
static int match_phrase(const toks_t *ts, int i, const char *phrase, int *ntok)
{
    char w[TOK_LEN];
    const char *p = phrase;
    int k = i;
    while (*p) {
        int l = 0;
        while (*p == ' ') p++;
        if (!*p) break;
        while (p[l] && p[l] != ' ' && l < TOK_LEN - 1) { w[l] = p[l]; l++; }
        w[l] = '\0';
        p += l;
        if (k >= ts->n || ts->t[k].type != T_WORD || strcmp(ts->t[k].text, w) != 0)
            return 0;
        k++;
    }
    *ntok = k - i;
    return k > i;
}

/* ------------------------------------------------------------------------ */
/* numbers, spoken or written                                                */
/* ------------------------------------------------------------------------ */

static const char *UNITS[] = { "zero", "one", "two", "three", "four", "five", "six",
    "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };
static const char *TENS[] = { "", "", "twenty", "thirty", "forty", "fifty", "sixty",
    "seventy", "eighty", "ninety" };

static int word_unit(const tok_t *k)
{
    int u;
    if (k->type != T_WORD) return -1;
    for (u = 0; u < 20; u++)
        if (strcmp(k->text, UNITS[u]) == 0) return u;
    if (strcmp(k->text, "oh") == 0) return 0;
    return -1;
}

static int word_tens(const tok_t *k)
{
    int t;
    if (k->type != T_WORD) return -1;
    for (t = 2; t < 10; t++)
        if (strcmp(k->text, TENS[t]) == 0) return t * 10;
    return -1;
}

typedef struct {
    int ok;
    double value;
    int decimals;
    int ntok;
    char text[TOK_LEN];     /* as it will be quoted back */
} num_t;

/* A number starting at token i: "0.8", ".8", "-0.3", "twenty two",
 * "point eight five", "zero point eight", "minus point three", "a hundred". */
static num_t read_number(const toks_t *ts, int i)
{
    num_t r;
    int k = i, neg = 0, have_int = 0;
    double v = 0;
    memset(&r, 0, sizeof(r));
    if (i < 0 || i >= ts->n)
        return r;
    if (ts->t[k].type == T_NUM) {
        r.ok = 1;
        r.value = ts->t[k].value;
        r.decimals = ts->t[k].decimals;
        r.ntok = 1;
        snprintf(r.text, sizeof(r.text), "%s", ts->t[k].text);
        return r;
    }
    if (is_w(ts, k, "minus|negative")) { neg = 1; k++; }
    if (k < ts->n && ts->t[k].type == T_WORD) {
        int t = word_tens(&ts->t[k]);
        int u = word_unit(&ts->t[k]);
        if (t > 0) {
            v = t; have_int = 1; k++;
            if (k < ts->n) {
                int u2 = word_unit(&ts->t[k]);
                if (u2 > 0 && u2 < 10) { v += u2; k++; }
            }
        } else if (u >= 0) {
            v = u; have_int = 1; k++;
        } else if (is_w(ts, k, "hundred") ||
                   (is_w(ts, k, "a") && is_w(ts, k + 1, "hundred"))) {
            k += is_w(ts, k, "a") ? 2 : 1;
            v = 100; have_int = 1;
        }
    }
    if (is_w(ts, k, "point")) {
        int d = 0;
        double scale = 0.1;
        k++;
        if (k < ts->n && ts->t[k].type == T_NUM && !strchr(ts->t[k].text, '.') &&
            ts->t[k].text[0] != '-') {
            const char *s = ts->t[k].text;
            for (; *s; s++, d++) { v += (*s - '0') * scale; scale /= 10; }
            k++;
        } else {
            while (k < ts->n) {
                int u = word_unit(&ts->t[k]);
                if (u < 0 || u > 9) break;
                v += u * scale; scale /= 10; d++; k++;
            }
        }
        if (d == 0)
            return r;               /* "point" alone is not a number */
        r.decimals = d;
    } else if (!have_int) {
        return r;
    }
    r.ok = 1;
    r.value = neg ? -v : v;
    r.ntok = k - i;
    snprintf(r.text, sizeof(r.text), "%.*f", r.decimals, r.value);
    return r;
}

/* The number after keyword position `kw`, allowing a few filler words in
 * between ("correlation at 0.8", "every correlation set to point eight"). */
static num_t number_after(const toks_t *ts, int kw, int *pct_follows)
{
    static const char *FILLER = "at|of|to|is|are|set|be|all|every|each|equal|equals|the|a|"
                                "around|about|uniform|uniformly|pairwise|of|with|by";
    int k = kw + 1, skipped = 0;
    num_t r;
    memset(&r, 0, sizeof(r));
    if (pct_follows) *pct_follows = 0;
    while (k < ts->n && skipped < 4 && is_w(ts, k, FILLER)) { k++; skipped++; }
    r = read_number(ts, k);
    if (r.ok && pct_follows) {
        int after = k + r.ntok;
        *pct_follows = (after < ts->n && ts->t[after].type == T_PCT) ||
                       is_w(ts, after, "percent|per|pct");
    }
    return r;
}

/* ------------------------------------------------------------------------ */
/* features                                                                  */
/* ------------------------------------------------------------------------ */

static const struct { const char *phrase; const char *ticker; } NAMES[] = {
    { "johnson and johnson", "JNJ" }, { "j and j", "JNJ" }, { "jnj", "JNJ" },
    { "j p morgan", "JPM" }, { "jp morgan", "JPM" }, { "jpmorgan", "JPM" },
    { "j p m", "JPM" }, { "jpm", "JPM" },
    { "amazon dot com", "AMZN" }, { "amazon com", "AMZN" }, { "amazon", "AMZN" },
    { "amzn", "AMZN" },
    { "microsoft", "MSFT" }, { "msft", "MSFT" },
    { "apple", "AAPL" }, { "aapl", "AAPL" },
};
#define N_NAMES ((int)(sizeof(NAMES) / sizeof(NAMES[0])))

typedef struct {
    int empty;
    kr_op_t op;
    int refuse;
    char refusal[KR_SAY_LEN];

    const kr_book_t *book;
    int book_ambiguous;
    int n_names;
    char names[KR_MAX_ASSETS][KR_TICKER_LEN];
    int too_many_names;
    int yes;

    int have_rho;
    double rho;
    int have_vol;
    double vol;         /* fraction */

    int n_weights;
    double weights_pct[KR_MAX_ASSETS + 1];

    int window_years;   /* -1 = not said */
    int window_months;

    int file_word[KR_MAX_FILES];    /* the utterance names file i */
} feat_t;

static void refuse(feat_t *f, const char *fmt, const char *a, const char *b)
{
    if (f->refuse)
        return;                     /* the first reason is the one to say */
    f->refuse = 1;
    snprintf(f->refusal, sizeof(f->refusal), fmt, a ? a : "", b ? b : "");
}

static int decimals_word(int d, char *buf, int len)
{
    static const char *W[] = { "no", "one", "two", "three", "four", "five", "six" };
    snprintf(buf, (size_t)len, "%s", d < 7 ? W[d] : "many");
    return d;
}

/* The distinctive words of each file's name: "fis" for fis_daily_closes.csv
 * against wiki_daily_closes_10yr.csv. */
static void file_words(const kr_file_t *f, char words[8][TOK_LEN], int *n)
{
    char tmp[KR_NAME_LEN];
    char *p, *save = NULL;
    *n = 0;
    snprintf(tmp, sizeof(tmp), "%s", f->name);
    for (p = strtok_r(tmp, "_.- ", &save); p && *n < 8; p = strtok_r(NULL, "_.- ", &save)) {
        int k;
        if (strcmp(p, "csv") == 0) continue;
        for (k = 0; p[k]; k++) p[k] = (char)tolower((unsigned char)p[k]);
        snprintf(words[(*n)++], TOK_LEN, "%s", p);
    }
}

static void find_features(const kr_context_t *ctx, const toks_t *ts, feat_t *f)
{
    int i, pct;
    num_t nr;
    char dbuf[16];

    memset(f, 0, sizeof(*f));
    f->window_years = -1;
    f->empty = (ts->n == 0);

    /* Covariances are never taken from speech, one cell or a whole matrix. */
    if (find_w(ts, "covariance|covariances") >= 0 ||
        (find_w(ts, "co") >= 0 && find_w(ts, "variance|variances") == find_w(ts, "co") + 1) ||
        (find_w(ts, "variance") >= 0 && is_w(ts, find_w(ts, "variance") + 1, "between")))
        refuse(f, "I won't take a covariance by voice; one wrong digit is invisible. "
                  "Give me the volatilities and one correlation, or name the file and "
                  "I will build it from the data.%s%s", NULL, NULL);

    /* Operation. A vol word followed by a number is a regime, not a verb. */
    for (i = 0; i < ts->n; i++) {
        if (is_w(ts, i, "simulate|simulation|simulated|simulating|forward|ahead|monte") ||
            (is_w(ts, i, "value") && is_w(ts, i + 1, "at") && is_w(ts, i + 2, "risk")))
            f->op = KR_OP_SIMULATE;
        else if (f->op == KR_OP_NONE &&
                 (is_w(ts, i, "variance|variants|variances|variant") ||
                  (is_w(ts, i, "volatility|vol") && !number_after(ts, i, NULL).ok)))
            f->op = KR_OP_VARIANCE;
    }

    /* Correlation. */
    for (i = 0; i < ts->n; i++) {
        if (!is_w(ts, i, "correlation|correlations|correlated|correlate|rho"))
            continue;
        nr = number_after(ts, i, &pct);
        if (!nr.ok)
            continue;
        if (nr.decimals > 2) {
            char msg[KR_SAY_LEN];
            decimals_word(nr.decimals, dbuf, sizeof(dbuf));
            snprintf(msg, sizeof(msg), "I heard a correlation of %s, %s decimals. I only "
                     "take two from speech, because a third digit is more likely a "
                     "mishearing than an intention. Say it again with two.", nr.text, dbuf);
            refuse(f, "%s%s", msg, NULL);
        } else if (nr.value < -1.0 || nr.value > 1.0) {
            refuse(f, "I heard a correlation of %s. Correlations run from minus one to "
                      "one.%s", nr.text, NULL);
        }
        f->have_rho = 1;
        f->rho = nr.value;
        break;
    }

    /* Every vol at N percent. "vowel" is how whisper hears "vol". */
    for (i = 0; i < ts->n; i++) {
        if (!is_w(ts, i, "vol|vols|volatility|volatilities|vowel|vowels"))
            continue;
        nr = number_after(ts, i, &pct);
        if (!nr.ok)
            continue;
        if (nr.decimals > 0 || nr.value <= 0 || nr.value > 150) {
            refuse(f, "I heard a volatility of %s. Give it in whole percent, like twenty-two "
                      "percent.%s", nr.text, NULL);
        }
        f->have_vol = 1;
        f->vol = nr.value / 100.0;
        break;
    }

    /* Names, longest phrase first at each position. */
    for (i = 0; i < ts->n;) {
        int j, used = 0;
        for (j = 0; j < N_NAMES; j++) {
            int nt, k, dup = 0;
            if (!match_phrase(ts, i, NAMES[j].phrase, &nt))
                continue;
            for (k = 0; k < f->n_names; k++)
                if (strcmp(f->names[k], NAMES[j].ticker) == 0) dup = 1;
            if (!dup) {
                if (f->n_names < KR_MAX_ASSETS)
                    snprintf(f->names[f->n_names++], KR_TICKER_LEN, "%s", NAMES[j].ticker);
                else
                    f->too_many_names = 1;
            }
            used = nt;
            break;
        }
        i += used ? used : 1;
    }

    /* A book, by one of its spoken forms, or "book" when only one exists. */
    for (i = 0; i < ctx->n_books && !f->book; i++) {
        int s, k, nt;
        for (s = 0; s < ctx->books[i].n_spoken && !f->book; s++) {
            toks_t phrase;
            char spoken[KR_NAME_LEN * 2];
            tokenise(ctx->books[i].spoken[s], &phrase);
            spoken[0] = '\0';
            for (k = 0; k < phrase.n; k++) {
                strncat(spoken, phrase.t[k].text, sizeof(spoken) - strlen(spoken) - 2);
                strcat(spoken, " ");
            }
            for (k = 0; k < ts->n && !f->book; k++)
                if (phrase.n > 0 && match_phrase(ts, k, spoken, &nt))
                    f->book = &ctx->books[i];
        }
    }
    if (!f->book && find_w(ts, "book|books") >= 0) {
        if (ctx->n_books == 1)
            f->book = &ctx->books[0];
        else if (ctx->n_books > 1)
            f->book_ambiguous = 1;
    }

    f->yes = find_w(ts, "yes|yeah|yep|correct|right") >= 0;

    /* Weights: "weights 25 25 20 15 15" -- at least two numbers, and not the
     * start of a window ("equal weights 10 years"). */
    i = find_w(ts, "weights|weight|weighted|weighting");
    if (i >= 0) {
        int k = i + 1, n = 0;
        if (is_w(ts, k, "of|at|are")) k++;
        while (k < ts->n && n <= KR_MAX_ASSETS) {
            nr = read_number(ts, k);
            if (!nr.ok) break;
            if (is_w(ts, k + nr.ntok, "year|years|yr|month|months")) break;
            if (nr.decimals > 2) {
                decimals_word(nr.decimals, dbuf, sizeof(dbuf));
                refuse(f, "I heard a weight of %s, %s decimals. Weights are whole percent.",
                       nr.text, dbuf);
            }
            f->weights_pct[n++] = nr.value;
            k += nr.ntok;
            if (k < ts->n && ts->t[k].type == T_PCT) k++;
            if (is_w(ts, k, "percent|and")) k++;
        }
        if (n >= 2)
            f->n_weights = n;
    }

    /* Window. "N years" / "N months" / "last year" / "twelve months" / "all".
     * "a year" straight after "forward" is a simulation horizon, not a window. */
    for (i = 0; i < ts->n; i++) {
        int horizon = is_w(ts, i - 1, "forward|ahead") || is_w(ts, i - 2, "forward|ahead");
        if (is_w(ts, i, "year|years|yr")) {
            nr = read_number(ts, i - 1);
            if (i >= 1 && ts->t[i - 1].type == T_NUM) nr = read_number(ts, i - 1);
            else if (i >= 2) {
                num_t two = read_number(ts, i - 2);
                if (two.ok && two.ntok == 2) nr = two;
            }
            if (nr.ok && nr.value >= 1 && nr.value <= 20 && nr.decimals == 0 && !horizon &&
                !is_w(ts, i - 1 - nr.ntok, "forward|ahead")) {
                f->window_years = (int)nr.value;
            } else if ((is_w(ts, i - 1, "last|past") ||
                        (is_w(ts, i - 1, "a|one") && is_w(ts, i - 2, "last|past"))) && !horizon) {
                f->window_years = 1;
            }
        }
        if (is_w(ts, i, "month|months")) {
            nr = read_number(ts, i - 1);
            if (i >= 2) {
                num_t two = read_number(ts, i - 2);
                if (two.ok && two.ntok == 2) nr = two;
            }
            if (nr.ok && nr.value >= 1 && nr.value <= 240 && !horizon)
                f->window_months = (int)nr.value;
        }
        if (is_w(ts, i, "whole|entire") && is_w(ts, i + 1, "file|history|thing"))
            f->window_years = 0;
    }

    /* Files, by their distinctive words. */
    for (i = 0; i < ctx->n_files; i++) {
        char w[8][TOK_LEN];
        int nw, a, b, c;
        file_words(&ctx->files[i], w, &nw);
        for (a = 0; a < nw; a++) {
            int shared = 0;
            for (b = 0; b < ctx->n_files && !shared; b++) {
                char w2[8][TOK_LEN];
                int nw2;
                if (b == i) continue;
                file_words(&ctx->files[b], w2, &nw2);
                for (c = 0; c < nw2; c++)
                    if (strcmp(w[a], w2[c]) == 0) shared = 1;
            }
            if (shared) continue;
            for (c = 0; c < ts->n; c++)
                if (ts->t[c].type == T_WORD && strcmp(ts->t[c].text, w[a]) == 0)
                    f->file_word[i] = 1;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* the spec                                                                  */
/* ------------------------------------------------------------------------ */

void kr_file_spoken(const kr_file_t *f, char *buf, int len)
{
    char w[8][TOK_LEN];
    int n, k;
    file_words(f, w, &n);
    if (n == 0) { snprintf(buf, (size_t)len, "the file %s", f->name); return; }
    if (strlen(w[0]) <= 3)
        for (k = 0; w[0][k]; k++) w[0][k] = (char)toupper((unsigned char)w[0][k]);
    snprintf(buf, (size_t)len, "the %s file", w[0]);
}

static const kr_file_t *file_by_name(const kr_context_t *ctx, const char *name)
{
    int i;
    for (i = 0; i < ctx->n_files; i++)
        if (strcmp(ctx->files[i].name, name) == 0)
            return &ctx->files[i];
    return NULL;
}

static int file_has(const kr_file_t *f, const char *ticker)
{
    int i;
    for (i = 0; i < f->n_tickers; i++)
        if (strcmp(f->tickers[i], ticker) == 0)
            return 1;
    return 0;
}

static void set_subject_book(kr_spec_t *sp, const kr_book_t *b)
{
    int i;
    sp->book = b;
    sp->n_assets = b->n_assets;
    for (i = 0; i < b->n_assets; i++)
        snprintf(sp->tickers[i], KR_TICKER_LEN, "%s", b->tickers[i]);
}

static void set_subject_names(kr_spec_t *sp, const feat_t *f)
{
    int i;
    sp->book = NULL;
    sp->n_assets = f->n_names;
    for (i = 0; i < f->n_names; i++)
        snprintf(sp->tickers[i], KR_TICKER_LEN, "%s", f->names[i]);
}

static void cat(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cat(char *buf, const char *fmt, ...)
{
    size_t l = strlen(buf);
    va_list ap;
    if (l >= KR_SAY_LEN - 1) return;
    va_start(ap, fmt);
    vsnprintf(buf + l, KR_SAY_LEN - l, fmt, ap);
    va_end(ap);
}

static const char *count_word(int n, int capital)
{
    static const char *W[] = { "zero", "one", "two", "three", "four", "five", "six",
                               "seven", "eight", "nine", "ten" };
    static const char *C[] = { "Zero", "One", "Two", "Three", "Four", "Five", "Six",
                               "Seven", "Eight", "Nine", "Ten" };
    if (n < 0 || n > 10) return capital ? "Many" : "many";
    return capital ? C[n] : W[n];
}

/* "The book — MSFT, AAPL, AMZN, JPM, JNJ, equal weights" or
 * "Five names, MSFT, AAPL, ..., weights 25, 25, 20, 15, 15 percent". */
static void say_subject(const kr_spec_t *sp, char *buf)
{
    int i;
    if (sp->book)
        cat(buf, "The book \xe2\x80\x94 ");
    else
        cat(buf, "%s names, ", count_word(sp->n_assets, 1));
    for (i = 0; i < sp->n_assets; i++)
        cat(buf, "%s%s", i ? ", " : "", sp->tickers[i]);
    if (sp->n_weights_spoken == 0)
        cat(buf, ", equal weights");
    else {
        cat(buf, ", weights ");
        for (i = 0; i < sp->n_weights_spoken; i++)
            cat(buf, "%s%g", i ? ", " : "", sp->weights_pct[i]);
        cat(buf, " percent%s", sp->weights_normalized ? ", normalised to 100" : "");
    }
}

static void say_data(const kr_spec_t *sp, char *buf)
{
    char fname[KR_NAME_LEN + 16];
    if (sp->regime == KR_REGIME_HYPOTHETICAL || !sp->file)
        return;
    kr_file_spoken(sp->file, fname, sizeof(fname));
    if (sp->window_months > 0)
        cat(buf, ", the last %s months of %s", count_word(sp->window_months, 0), fname);
    else if (sp->window_years == 1)
        cat(buf, ", the last year of %s", fname);
    else if (sp->window_years > 1)
        cat(buf, ", %s years of %s", count_word(sp->window_years, 0), fname);
    else
        cat(buf, ", the whole of %s", fname);
}

static void say_regime_and_op(const kr_spec_t *sp, char *buf)
{
    if (sp->regime == KR_REGIME_STRESSED)
        cat(buf, ". Every correlation set to %.2f, historical vols kept: a hypothetical "
                 "regime", sp->rho);
    else if (sp->regime == KR_REGIME_HYPOTHETICAL)
        cat(buf, ". Every vol at %g percent, every correlation %.2f: a hypothetical "
                 "regime, no file", sp->vol * 100.0, sp->rho);
    if (sp->op == KR_OP_SIMULATE)
        cat(buf, ". Simulating one year forward, ten thousand paths, seed %d.", sp->seed);
    else
        cat(buf, ". Portfolio variance.");
}

void kr_describe(const kr_spec_t *sp, char *buf, int len)
{
    char tmp[KR_SAY_LEN];
    tmp[0] = '\0';
    say_subject(sp, tmp);
    say_data(sp, tmp);
    snprintf(buf, (size_t)len, "%s", tmp);
}

static void ask(kr_session_t *s, kr_result_t *out, const kr_spec_t *sp, kr_slot_t slot,
                int have_subject)
{
    out->outcome = KR_ASK;
    s->missing = slot;
    s->spec = *sp;
    s->have_subject = have_subject;
}

/* Finish a draft: validate, fill defaults, or stop at the first gap. */
static void complete(const kr_context_t *ctx, kr_session_t *s, kr_spec_t *sp,
                     int have_subject, int file_hint, int file_hint_other,
                     kr_result_t *out)
{
    int i;
    memset(out, 0, sizeof(*out));

    /* 1. The subject. */
    if (!have_subject) {
        if (ctx->n_books == 1) {
            snprintf(out->say, sizeof(out->say), "On the book \xe2\x80\x94 ");
            for (i = 0; i < ctx->books[0].n_assets; i++)
                cat(out->say, "%s%s", i ? ", " : "", ctx->books[0].tickers[i]);
            cat(out->say, " \xe2\x80\x94 or on other names?");
        } else if (ctx->n_books > 1) {
            snprintf(out->say, sizeof(out->say), "Which book: ");
            for (i = 0; i < ctx->n_books; i++)
                cat(out->say, "%s%s", i ? (i == ctx->n_books - 1 ? " or " : ", ") : "",
                    ctx->books[i].name);
            cat(out->say, "?");
        } else {
            snprintf(out->say, sizeof(out->say), "Which names?");
        }
        ask(s, out, sp, KR_SLOT_SUBJECT, 0);
        return;
    }
    if (sp->n_assets < 2) {
        out->outcome = KR_REFUSE;
        snprintf(out->say, sizeof(out->say), "I only heard %s%s. A portfolio needs at least "
                 "two names, or say the book.", sp->n_assets ? "one name, " : "no names",
                 sp->n_assets ? sp->tickers[0] : "");
        kr_session_init(s);
        return;
    }

    /* 2. The operation. A regime without a verb is a variance, by rule. */
    if (sp->op == KR_OP_NONE) {
        if (sp->regime != KR_REGIME_HISTORICAL) {
            sp->op = KR_OP_VARIANCE;
        } else {
            out->say[0] = '\0';
            say_subject(sp, out->say);
            cat(out->say, ". I didn't catch what to run on it: portfolio variance, or a "
                          "simulation forward?");
            ask(s, out, sp, KR_SLOT_OPERATION, 1);
            return;
        }
    }

    /* 3. A hypothetical regime needs its correlation. */
    if (sp->regime == KR_REGIME_HYPOTHETICAL && !sp->have_rho) {
        snprintf(out->say, sizeof(out->say), "Every vol at %g percent. And what correlation "
                 "between them?", sp->vol * 100.0);
        ask(s, out, sp, KR_SLOT_CORRELATION, 1);
        return;
    }

    /* 4. Weights. */
    sp->weights_normalized = 0;
    if (sp->n_weights_spoken == 0) {
        sp->weights_equal = 1;
        for (i = 0; i < sp->n_assets; i++) sp->weights[i] = 1.0 / sp->n_assets;
    } else {
        double sum = 0;
        if (sp->n_weights_spoken != sp->n_assets) {
            out->outcome = KR_REFUSE;
            snprintf(out->say, sizeof(out->say), "I heard %d weights for %d names. Give me "
                     "one weight per name, or say equal.", sp->n_weights_spoken, sp->n_assets);
            kr_session_init(s);
            return;
        }
        for (i = 0; i < sp->n_assets; i++) sum += sp->weights_pct[i];
        if (fabs(sum - 100.0) > 1.0 + 1e-9) {
            out->outcome = KR_REFUSE;
            snprintf(out->say, sizeof(out->say), "Those weights sum to %g percent. Give me %s "
                     "that sum to 100, or say equal.", sum, count_word(sp->n_assets, 0));
            kr_session_init(s);
            return;
        }
        sp->weights_equal = 0;
        sp->weights_normalized = fabs(sum - 100.0) > 1e-9;
        for (i = 0; i < sp->n_assets; i++) sp->weights[i] = sp->weights_pct[i] / sum;
    }

    /* 5. The file. */
    if (sp->regime == KR_REGIME_HYPOTHETICAL) {
        sp->file = NULL;
    } else if (sp->book) {
        const kr_file_t *bf = file_by_name(ctx, sp->book->file);
        if (!bf) {
            out->outcome = KR_REFUSE;
            snprintf(out->say, sizeof(out->say), "The book is defined on %s, and that file "
                     "is not on the phone.", sp->book->file);
            kr_session_init(s);
            return;
        }
        if (file_hint_other) {
            char fname[KR_NAME_LEN + 16];
            out->outcome = KR_REFUSE;
            kr_file_spoken(bf, fname, sizeof(fname));
            snprintf(out->say, sizeof(out->say), "The book is defined on %s. To use another "
                     "file, name the stocks instead of the book.", fname);
            kr_session_init(s);
            return;
        }
        sp->file = bf;
        if (sp->window_years < 0 && sp->window_months == 0)
            sp->window_years = sp->book->window_years;
    } else if (!sp->file) {
        const kr_file_t *cand[KR_MAX_FILES];
        int nc = 0, j;
        for (i = 0; i < ctx->n_files; i++) {
            int all = 1;
            for (j = 0; j < sp->n_assets; j++)
                if (!file_has(&ctx->files[i], sp->tickers[j])) all = 0;
            if (all) cand[nc++] = &ctx->files[i];
        }
        if (nc == 0) {
            out->outcome = KR_REFUSE;
            snprintf(out->say, sizeof(out->say), "No file on the phone has prices for all of ");
            for (j = 0; j < sp->n_assets; j++)
                cat(out->say, "%s%s", j ? ", " : "", sp->tickers[j]);
            cat(out->say, ".");
            kr_session_init(s);
            return;
        }
        if (file_hint >= 0) {
            for (i = 0; i < nc; i++)
                if (cand[i] == &ctx->files[file_hint]) sp->file = cand[i];
        }
        if (!sp->file && nc == 1)
            sp->file = cand[0];
        if (!sp->file) {
            char fname[KR_NAME_LEN + 16];
            snprintf(out->say, sizeof(out->say), "%s files on the phone carry those names: ",
                     count_word(nc, 1));
            for (i = 0; i < nc; i++) {
                kr_file_spoken(cand[i], fname, sizeof(fname));
                cat(out->say, "%s%s", i ? (i == nc - 1 ? " and " : ", ") : "", fname);
            }
            cat(out->say, ". Which one?");
            ask(s, out, sp, KR_SLOT_FILE, 1);
            return;
        }
    }

    /* 6. The window, in observations counted back from the newest row. */
    if (sp->window_months > 0)
        sp->max_obs = sp->window_months * DAYS_PER_MONTH;
    else if (sp->window_years > 0)
        sp->max_obs = sp->window_years * DAYS_PER_YEAR;
    else
        sp->max_obs = 0;
    if (sp->max_obs > MAX_OBS) sp->max_obs = MAX_OBS;
    if (sp->window_years < 0) sp->window_years = 0;

    if (sp->op == KR_OP_SIMULATE) {
        sp->seed = KR_DEFAULT_SEED;
        sp->n_simulations = KR_DEFAULT_SIMULATIONS;
    }

    /* 7. Run it, after saying what will run. */
    out->outcome = KR_RUN;
    out->spec = *sp;
    out->say[0] = '\0';
    say_subject(sp, out->say);
    say_data(sp, out->say);
    say_regime_and_op(sp, out->say);
    kr_session_init(s);
}

void kr_session_init(kr_session_t *s)
{
    memset(s, 0, sizeof(*s));
    s->missing = KR_SLOT_NONE;
    s->spec.window_years = -1;
}

void kr_resolve(const kr_context_t *ctx, kr_session_t *s, const char *transcript,
                kr_result_t *out)
{
    toks_t ts;
    feat_t f;
    kr_spec_t sp;
    int i, have_subject, file_hint = -1, file_hint_other = 0, n_hints = 0;

    memset(out, 0, sizeof(*out));
    tokenise(transcript ? transcript : "", &ts);
    find_features(ctx, &ts, &f);

    if (f.empty) {
        out->outcome = KR_SILENT;       /* a blank clip keeps any pending question */
        return;
    }
    if (f.refuse) {
        out->outcome = KR_REFUSE;
        snprintf(out->say, sizeof(out->say), "%s", f.refusal);
        kr_session_init(s);
        return;
    }
    if (f.too_many_names) {
        out->outcome = KR_REFUSE;
        snprintf(out->say, sizeof(out->say), "That is more than %d names. Name at most %d, or "
                 "say the book.", KR_MAX_ASSETS, KR_MAX_ASSETS);
        kr_session_init(s);
        return;
    }
    for (i = 0; i < ctx->n_files; i++)
        if (f.file_word[i]) { file_hint = i; n_hints++; }
    if (n_hints > 1) file_hint = -1;

    /* A pending question, answered -- unless this is a new request in full. */
    if (s->missing != KR_SLOT_NONE) {
        int fresh = f.op != KR_OP_NONE &&
                    (f.book || f.n_names > 0 || f.have_vol || f.have_rho);
        if (!fresh) {
            kr_slot_t slot = s->missing;
            int filled = 0;
            sp = s->spec;
            have_subject = s->have_subject;
            if (slot == KR_SLOT_SUBJECT) {
                if (f.book) { set_subject_book(&sp, f.book); filled = 1; }
                else if (f.n_names > 0) { set_subject_names(&sp, &f); filled = 1; }
                else if (f.yes && ctx->n_books == 1) {
                    set_subject_book(&sp, &ctx->books[0]);
                    filled = 1;
                }
                if (filled) have_subject = 1;
            } else if (slot == KR_SLOT_OPERATION) {
                if (f.op != KR_OP_NONE) { sp.op = f.op; filled = 1; }
            } else if (slot == KR_SLOT_FILE) {
                if (file_hint >= 0) filled = 1;
            } else if (slot == KR_SLOT_CORRELATION) {
                if (f.have_rho) { sp.rho = f.rho; sp.have_rho = 1; filled = 1; }
            }
            if (filled) {
                if (f.have_rho && slot != KR_SLOT_CORRELATION) {
                    sp.rho = f.rho;
                    sp.have_rho = 1;
                    if (sp.regime == KR_REGIME_HISTORICAL) sp.regime = KR_REGIME_STRESSED;
                }
                complete(ctx, s, &sp, have_subject, file_hint, 0, out);
                return;
            }
            /* Not an answer and not a request: say nothing, keep the question. */
            if (f.op == KR_OP_NONE && !f.book && f.n_names == 0 && !f.have_rho &&
                !f.have_vol) {
                out->outcome = KR_SILENT;
                return;
            }
        }
        kr_session_init(s);
    }

    /* A fresh request. Nothing calculable at all is silence. */
    if (f.op == KR_OP_NONE && !f.book && !f.book_ambiguous && f.n_names == 0 &&
        !f.have_rho && !f.have_vol) {
        out->outcome = KR_SILENT;
        return;
    }

    memset(&sp, 0, sizeof(sp));
    sp.window_years = -1;
    sp.op = f.op;
    have_subject = 0;
    if (f.book && f.n_names == 0) {
        set_subject_book(&sp, f.book);
        have_subject = 1;
    } else if (f.n_names > 0) {
        set_subject_names(&sp, &f);
        have_subject = 1;
    }
    if (f.have_vol) {
        sp.regime = KR_REGIME_HYPOTHETICAL;
        sp.vol = f.vol;
    } else if (f.have_rho) {
        sp.regime = KR_REGIME_STRESSED;
    }
    sp.have_rho = f.have_rho;
    sp.rho = f.rho;
    sp.n_weights_spoken = f.n_weights;
    for (i = 0; i < f.n_weights; i++) sp.weights_pct[i] = f.weights_pct[i];
    sp.window_years = f.window_years;
    sp.window_months = f.window_months;
    if (sp.book && file_hint >= 0 && strcmp(ctx->files[file_hint].name, sp.book->file) != 0)
        file_hint_other = 1;
    complete(ctx, s, &sp, have_subject, file_hint, file_hint_other, out);
}
