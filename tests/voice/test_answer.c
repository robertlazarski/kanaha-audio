/*
 * Kanaha Audio - answer tests (host build: make -C tests/voice check)
 * Licensed under the Apache License, Version 2.0
 *
 * The numbers are what the calcs phone returned on 2026-09-22 for the
 * rehearsed requests; the expected sentences are the ones the orchestrator
 * prompt specifies for them.
 */

#include "../../kanaha-audio-app/app/src/main/cpp/voice/kanaha_answer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
static char A[KA_ANSWER_LEN], S[KR_SAY_LEN], T[KA_TRACE_LEN];

static void has(const char *label, const char *text, const char *want)
{
    checks++;
    if (!strstr(text, want)) {
        failures++;
        printf("  FAIL  %s\n        wanted \"%s\"\n        in     \"%s\"\n", label, want, text);
    }
}

static void ok(const char *label, int cond)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", label); }
}

static kr_book_t BOOK;
static kr_file_t FIS;

static kr_spec_t book_spec(kr_op_t op, kr_regime_t regime)
{
    static const char *t[] = { "MSFT", "AAPL", "AMZN", "JPM", "JNJ" };
    kr_spec_t sp;
    int i;
    memset(&sp, 0, sizeof(sp));
    sp.op = op;
    sp.regime = regime;
    sp.book = &BOOK;
    sp.n_assets = 5;
    for (i = 0; i < 5; i++) { snprintf(sp.tickers[i], KR_TICKER_LEN, "%s", t[i]); sp.weights[i] = 0.2; }
    sp.weights_equal = 1;
    sp.file = regime == KR_REGIME_HYPOTHETICAL ? NULL : &FIS;
    sp.window_years = 10;
    sp.max_obs = 2520;
    sp.seed = 12345;
    sp.n_simulations = 10000;
    return sp;
}

static kc_result_t fis_result(void)
{
    /* per-asset vols of the five on the FIS file, 2016-05-06..2026-05-06 */
    static const double v[] = { 0.2688, 0.2994, 0.3401, 0.2692, 0.1636 };
    kc_result_t r;
    int i;
    memset(&r, 0, sizeof(r));
    r.ok = 1;
    snprintf(r.first_date, sizeof(r.first_date), "2016-05-06");
    snprintf(r.last_date, sizeof(r.last_date), "2026-05-06");
    r.n_obs = 2508;
    for (i = 0; i < 5; i++) r.vols[i] = v[i];
    r.sigma_trace = 0.3702;
    snprintf(r.tools, sizeof(r.tools), "covarianceFromCsv,portfolioVariance");
    snprintf(r.request_id, sizeof(r.request_id), "kv-pv-hist-demo5");
    return r;
}

int main(void)
{
    kr_spec_t sp;
    kc_result_t r;

    snprintf(BOOK.name, sizeof(BOOK.name), "demo5");
    snprintf(FIS.name, sizeof(FIS.name), "fis_daily_closes.csv");

    printf("variance, historical\n");
    sp = book_spec(KR_OP_VARIANCE, KR_REGIME_HISTORICAL);
    r = fis_result();
    r.variance = 0.039744; r.volatility = 0.19936; r.calc_time_us = 1;
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("answer opens with the book, expanded", A,
        "The book \xe2\x80\x94 MSFT, AAPL, AMZN, JPM, JNJ, equal weights, ten years of the FIS file.");
    has("answer carries the numbers", A, "Variance 0.039744, volatility 19.94 percent");
    has("weighted average vol is ours to compute", A, "weighted average vol 26.82 percent");
    has("and the diversification benefit", A, "diversification benefit 25.7 percent");
    has("the time, in the units it came back in", A, "One microsecond in C.");
    has("SAY carries the result and the comparison", S,
        "The book comes out at 19.94 percent volatility, against a weighted average of 26.8");
    ok("SAY is under 180 characters", strlen(S) <= KA_SAY_MAX);
    ok("SAY has no date or file name", !strstr(S, "2016") && !strstr(S, "FIS") && !strstr(S, ".csv"));
    has("trace names the book and the expansion", T, "SPEC | book=demo5 | assets=MSFT,AAPL,AMZN,JPM,JNJ");
    has("trace is replayable", T, "source=fis_daily_closes.csv:*_AdjClose");
    has("trace window", T, "window=2016-05-06..2026-05-06 | obs=2508");
    has("trace sigma and vol", T, "sigma_trace=0.3702 | vol=0.1994");

    printf("variance, stressed and hypothetical\n");
    sp = book_spec(KR_OP_VARIANCE, KR_REGIME_STRESSED);
    sp.rho = 0.8;
    r.variance = 0.0605; r.volatility = 0.24597; r.calc_time_us = 4;
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("stressed answer says it is a regime", A, "every correlation at 0.80: a hypothetical regime");
    has("stressed SAY", S, "At a correlation of 0.80, the book comes out at 24.60 percent");
    has("four microseconds", A, "Four microseconds in C.");
    has("trace records the regime", T, "regime=stressed rho=0.80");

    sp = book_spec(KR_OP_VARIANCE, KR_REGIME_HYPOTHETICAL);
    sp.vol = 0.22; sp.rho = 0.5;
    r = fis_result();
    r.variance = 0.02904; r.volatility = 0.17041; r.calc_time_us = 1;
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("hypothetical: no file in the answer", A, "no file.");
    ok("hypothetical: no dates in the answer", !strstr(A, "2016"));
    has("hypothetical: weighted average is the spoken vol", A, "weighted average vol 22.00 percent");
    has("hypothetical SAY", S, "In that hypothetical regime, the book comes out at 17.04 percent");
    has("hypothetical trace has no source", T, "source=- | window=- | obs=-");

    printf("simulation\n");
    sp = book_spec(KR_OP_SIMULATE, KR_REGIME_HISTORICAL);
    r = fis_result();
    r.initial_value = 1000000; r.var_99 = 331846.2; r.var_95 = 234252.1; r.cvar_95 = 293059.4;
    r.max_drawdown = 0.537; r.prob_profit = 0.618; r.volatility = 0.19936; r.calc_time_us = 491234;
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("VaR with thousands separators", A, "99 percent value at risk 331,846, 95 percent 234,252");
    has("the model's parameters", A, "ten thousand paths, seed 12345: on 1,000,000");
    has("milliseconds, not rounded up", A, "491 milliseconds on the phone.");
    has("simulation SAY", S, "Simulated a year forward, the book has a 99 percent value at risk of "
        "331,846 on a million, with 62 percent of paths in profit.");
    has("trace seed", T, "seed=12345");
    sp.regime = KR_REGIME_STRESSED; sp.rho = 0.8; r.var_99 = 408081.09;
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("stressed simulation SAY", S, "At a correlation of 0.80, a year forward, the book has a 99 "
        "percent value at risk of 408,081 on a million.");

    printf("names, not a book\n");
    sp = book_spec(KR_OP_VARIANCE, KR_REGIME_HISTORICAL);
    sp.book = NULL; sp.n_assets = 4;
    snprintf(sp.tickers[1], KR_TICKER_LEN, "AMZN"); snprintf(sp.tickers[2], KR_TICKER_LEN, "JPM");
    snprintf(sp.tickers[3], KR_TICKER_LEN, "JNJ");
    sp.weights[0] = sp.weights[1] = sp.weights[2] = sp.weights[3] = 0.25;
    r = fis_result(); r.variance = 0.04; r.volatility = 0.2; r.calc_time_us = 2;
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("names answer", A, "Four names, MSFT, AMZN, JPM, JNJ, equal weights");
    has("plural verb", S, "Those four names come out at 20.00 percent");
    ok("no book in the trace", !strstr(T, "book="));

    printf("failures\n");
    sp = book_spec(KR_OP_VARIANCE, KR_REGIME_HISTORICAL);
    r = fis_result();
    r.variance = 10.0; r.volatility = 3.16; r.calc_time_us = 1;
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("a vol over 100 percent is not reported as a result", A, "I won't report it as a result");
    ok("and its number is not in SAY", !strstr(S, "316"));
    memset(&r, 0, sizeof(r));
    snprintf(r.error, sizeof(r.error), "The calcs phone refused composeCovariance: correlation "
             "matrix is not positive semi-definite at index 3; a uniform correlation must be at "
             "least -0.25 for five assets, so this is not a covariance matrix and was refused.");
    ka_answer(&sp, &r, A, sizeof(A), S, sizeof(S), T, sizeof(T));
    has("a refusal is quoted", A, "not positive semi-definite at index 3");
    ok("a long refusal is cut to fit SAY", strlen(S) <= KA_SAY_MAX && S[strlen(S) - 1] == '.');
    ok("no trace for a failure", T[0] == '\0');

    printf("%s: %d of %d checks failed\n", failures ? "FAILED" : "PASSED", failures, checks);
    return failures ? 1 : 0;
}
