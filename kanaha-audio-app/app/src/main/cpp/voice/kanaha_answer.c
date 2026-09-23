/*
 * Kanaha Audio - answers to resolved requests
 * Licensed under the Apache License, Version 2.0
 */

#include "kanaha_answer.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void cat(char *buf, int len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cat(char *buf, int len, const char *fmt, ...)
{
    size_t l = strlen(buf);
    va_list ap;
    if ((int)l >= len - 1) return;
    va_start(ap, fmt);
    vsnprintf(buf + l, (size_t)len - l, fmt, ap);
    va_end(ap);
}

static const char *word(int n, int capital)
{
    static const char *W[] = { "zero", "one", "two", "three", "four", "five", "six",
                               "seven", "eight", "nine", "ten" };
    static const char *C[] = { "Zero", "One", "Two", "Three", "Four", "Five", "Six",
                               "Seven", "Eight", "Nine", "Ten" };
    return (n >= 0 && n <= 10) ? (capital ? C[n] : W[n]) : "";
}

/* "331,846" */
static void money(double v, char *buf, int len)
{
    char digits[32];
    int n, i, o = 0, lead;
    snprintf(digits, sizeof(digits), "%.0f", fabs(v));
    n = (int)strlen(digits);
    lead = n % 3 ? n % 3 : 3;
    if (v < 0 && o < len - 1) buf[o++] = '-';
    for (i = 0; i < n && o < len - 2; i++) {
        if (i && (i - lead) % 3 == 0) buf[o++] = ',';
        buf[o++] = digits[i];
    }
    buf[o] = '\0';
}

/* The calculation's own time, in the units it lands in: "One microsecond",
 * "153 microseconds", "392 milliseconds". Never rounded up for effect. */
static void duration(long us, char *buf, int len)
{
    if (us < 1) us = 1;
    if (us < 1000) {
        if (us <= 10)
            snprintf(buf, (size_t)len, "%s microsecond%s", word((int)us, 1), us == 1 ? "" : "s");
        else
            snprintf(buf, (size_t)len, "%ld microseconds", us);
    } else if (us < 10000000) {
        long ms = (us + 500) / 1000;
        snprintf(buf, (size_t)len, "%ld millisecond%s", ms, ms == 1 ? "" : "s");
    } else {
        snprintf(buf, (size_t)len, "%.1f seconds", us / 1e6);
    }
}

double ka_weighted_vol(const kr_spec_t *sp, const kc_result_t *r)
{
    double w = 0;
    int i;
    for (i = 0; i < sp->n_assets; i++)
        w += sp->weights[i] * (sp->regime == KR_REGIME_HYPOTHETICAL ? sp->vol : r->vols[i]);
    return w;
}

/* "The book" / "those five names", and whether the verb takes an s. */
static const char *subject(const kr_spec_t *sp, int capital, int *plural)
{
    static char buf[48];
    if (sp->book) {
        *plural = 0;
        return capital ? "The book" : "the book";
    }
    *plural = 1;
    snprintf(buf, sizeof(buf), "%s %s names", capital ? "Those" : "those",
             word(sp->n_assets, 0));
    return buf;
}

static void regime_clause(const kr_spec_t *sp, char *buf, int len)
{
    if (sp->regime == KR_REGIME_STRESSED)
        cat(buf, len, ", every correlation at %.2f: a hypothetical regime", sp->rho);
    else if (sp->regime == KR_REGIME_HYPOTHETICAL)
        cat(buf, len, ", every vol at %g percent and every correlation at %.2f: a "
                      "hypothetical regime, no file", sp->vol * 100.0, sp->rho);
}

/* Cut to the last whole sentence or clause that fits, never mid-word. */
static void fit_say(char *say, int say_len)
{
    int max = say_len - 1 < KA_SAY_MAX ? say_len - 1 : KA_SAY_MAX;
    int i;
    if ((int)strlen(say) <= max) return;
    for (i = max; i > 0; i--)
        if (say[i] == '.' || say[i] == ',') { say[i] = '.'; say[i + 1] = '\0'; return; }
    say[max] = '\0';
}

static void trace_line(const kr_spec_t *sp, const kc_result_t *r, char *t, int len)
{
    int i;
    t[0] = '\0';
    cat(t, len, "SPEC");
    if (sp->book) cat(t, len, " | book=%s", sp->book->name);
    cat(t, len, " | assets=");
    for (i = 0; i < sp->n_assets; i++) cat(t, len, "%s%s", i ? "," : "", sp->tickers[i]);
    cat(t, len, " | weights=");
    for (i = 0; i < sp->n_assets; i++) cat(t, len, "%s%g", i ? "," : "", sp->weights[i] * 100.0);
    if (sp->file) {
        cat(t, len, " | source=%s:*_AdjClose | window=%s..%s | obs=%d | periods_per_year=252",
            sp->file->name, r->first_date, r->last_date, r->n_obs);
        if (sp->max_obs > 0) cat(t, len, " | max_obs=%d", sp->max_obs);
    } else {
        cat(t, len, " | source=- | window=- | obs=-");
    }
    if (sp->regime == KR_REGIME_STRESSED) cat(t, len, " | regime=stressed rho=%.2f", sp->rho);
    if (sp->regime == KR_REGIME_HYPOTHETICAL)
        cat(t, len, " | regime=hypothetical vol=%.2f rho=%.2f", sp->vol, sp->rho);
    cat(t, len, " | tools=%s | target=calcs | confirmed=n/a | request_id=%s", r->tools,
        r->request_id);
    if (sp->op == KR_OP_SIMULATE) cat(t, len, " | seed=%d", sp->seed);
    else cat(t, len, " | seed=-");
    cat(t, len, " | sigma_trace=%.4f | vol=%.4f", r->sigma_trace, r->volatility);
}

void ka_answer(const kr_spec_t *sp, const kc_result_t *r,
               char *answer, int answer_len, char *say, int say_len,
               char *trace, int trace_len)
{
    char desc[KR_SAY_LEN], took[48], v99[32], v95[32], cv[32], iv[32];
    const char *subj;
    int plural;
    double wavg, benefit;

    answer[0] = say[0] = trace[0] = '\0';
    if (!r->ok) {
        snprintf(answer, (size_t)answer_len, "%s", r->error);
        snprintf(say, (size_t)say_len, "%s", r->error);
        fit_say(say, say_len);
        return;
    }

    kr_describe(sp, desc, sizeof(desc));
    snprintf(answer, (size_t)answer_len, "%s", desc);
    regime_clause(sp, answer, answer_len);
    cat(answer, answer_len, ".");
    if (sp->file)
        cat(answer, answer_len, " %s to %s, %d observations.", r->first_date, r->last_date,
            r->n_obs);
    duration(r->calc_time_us, took, sizeof(took));
    trace_line(sp, r, trace, trace_len);

    /* A volatility over 100 percent here means the matrix was annualised
     * twice (the prompt's own warning); it is not reported as a result. */
    if (r->volatility > 1.0) {
        snprintf(answer, (size_t)answer_len, "The calculation came back at %.0f percent "
                 "volatility, which means the matrix basis is wrong, so I won't report it as "
                 "a result.", r->volatility * 100.0);
        snprintf(say, (size_t)say_len, "That came back above one hundred percent volatility, "
                 "so something is wrong with the matrix and I won't report it.");
        fit_say(say, say_len);
        return;
    }

    subj = subject(sp, 0, &plural);
    if (sp->op == KR_OP_VARIANCE) {
        wavg = ka_weighted_vol(sp, r);
        benefit = wavg > 0 ? 1.0 - r->volatility / wavg : 0;
        cat(answer, answer_len, " Variance %.6f, volatility %.2f percent, weighted average vol "
            "%.2f percent, diversification benefit %.1f percent. %s in C.", r->variance,
            r->volatility * 100.0, wavg * 100.0, benefit * 100.0, took);

        if (sp->regime == KR_REGIME_HISTORICAL) {
            snprintf(say, (size_t)say_len, "%s come%s out at %.2f percent volatility, against a "
                     "weighted average of %.1f, so diversification is taking %.0f percent off.",
                     subject(sp, 1, &plural), plural ? "" : "s", r->volatility * 100.0,
                     wavg * 100.0, benefit * 100.0);
        } else if (sp->regime == KR_REGIME_STRESSED) {
            snprintf(say, (size_t)say_len, "At a correlation of %.2f, %s come%s out at %.2f "
                     "percent volatility, against a weighted average of %.1f.", sp->rho, subj,
                     plural ? "" : "s", r->volatility * 100.0, wavg * 100.0);
        } else {
            snprintf(say, (size_t)say_len, "In that hypothetical regime, %s come%s out at %.2f "
                     "percent volatility, against %g for each name on its own.", subj,
                     plural ? "" : "s", r->volatility * 100.0, sp->vol * 100.0);
        }
    } else {
        money(r->var_99, v99, sizeof(v99));
        money(r->var_95, v95, sizeof(v95));
        money(r->cvar_95, cv, sizeof(cv));
        money(r->initial_value, iv, sizeof(iv));
        char paths[32];
        if (sp->n_simulations == 10000)
            snprintf(paths, sizeof(paths), "ten thousand");
        else
            money(sp->n_simulations, paths, sizeof(paths));
        cat(answer, answer_len, " Simulated one year forward, %s paths, seed %d: on %s, 99 "
            "percent value at risk %s, 95 percent %s, expected shortfall at 95 percent %s, max "
            "drawdown %.1f percent, %.1f percent of paths in profit. Portfolio volatility %.2f "
            "percent. %s on the phone.", paths,
            sp->seed, iv, v99, v95, cv, r->max_drawdown * 100.0, r->prob_profit * 100.0,
            r->volatility * 100.0, took);
        if (sp->regime == KR_REGIME_STRESSED)
            snprintf(say, (size_t)say_len, "At a correlation of %.2f, a year forward, %s ha%s a "
                     "99 percent value at risk of %s on a million.", sp->rho, subj,
                     plural ? "ve" : "s", v99);
        else if (sp->regime == KR_REGIME_HYPOTHETICAL)
            snprintf(say, (size_t)say_len, "In that hypothetical regime, a year forward, %s ha%s "
                     "a 99 percent value at risk of %s on a million.", subj, plural ? "ve" : "s",
                     v99);
        else
            snprintf(say, (size_t)say_len, "Simulated a year forward, %s ha%s a 99 percent value "
                     "at risk of %s on a million, with %.0f percent of paths in profit.", subj,
                     plural ? "ve" : "s", v99, r->prob_profit * 100.0);
    }
    fit_say(say, say_len);
}
