/*
 * Kanaha Audio - answers to resolved requests
 * Licensed under the Apache License, Version 2.0
 *
 * Turns what the calcs phone returned into three lines:
 *
 *   answer  the written answer. Opens by saying what was computed -- for a
 *           book, every holding by ticker -- then the numbers, then the time
 *           the calculation took, in the units it came back in.
 *   say     one sentence under 180 characters for the speech synthesiser: the
 *           result and the one comparison that matters. No dates, no file
 *           names, no trace.
 *   trace   the SPEC | line (kanaha-voice-orchestrator-prompt.md, "Leave a
 *           trace"), which kanaha-spec-replay.py reads back and re-runs.
 *
 * Every number comes from the response except two the prompt defines as ours:
 * the weighted average volatility (sum of w_i * vol_i) and the
 * diversification benefit (1 - portfolio vol / weighted average vol).
 *
 * Pure C: no I/O, no allocation.
 */

#ifndef KANAHA_ANSWER_H
#define KANAHA_ANSWER_H

#include "kanaha_resolver.h"

#define KA_ANSWER_LEN 768
#define KA_SAY_MAX    180
#define KA_TRACE_LEN  768

/* What the calcs phone returned for one spec (kanaha_calc.c fills it). */
typedef struct {
    int ok;
    char error[KR_SAY_LEN];         /* sayable: the service's refusal, or why it was unreachable */

    /* where the matrix came from (historical and stressed) */
    char first_date[16];
    char last_date[16];
    int n_obs;
    double vols[KR_MAX_ASSETS];     /* annualised, per asset, as used */
    double sigma_trace;             /* sum of the matrix diagonal */

    /* portfolioVariance */
    double variance;
    double volatility;

    /* monteCarlo */
    double initial_value;
    double var_95, var_99, cvar_95;
    double max_drawdown;
    double prob_profit;

    long calc_time_us;              /* of the final operation, from the response */
    long round_trip_ms;             /* every call, measured on this phone */
    char tools[128];                /* "covarianceFromCsv,portfolioVariance" */
    int vols_reused;                /* stressed: the vols came from an earlier read, no file opened */
    char request_id[96];            /* "kv-<op>-<regime>-<book>" */
} kc_result_t;

void ka_answer(const kr_spec_t *sp, const kc_result_t *r,
               char *answer, int answer_len,
               char *say, int say_len,
               char *trace, int trace_len);

/* Weighted average of the per-asset vols, as a fraction. */
double ka_weighted_vol(const kr_spec_t *sp, const kc_result_t *r);

#endif /* KANAHA_ANSWER_H */
