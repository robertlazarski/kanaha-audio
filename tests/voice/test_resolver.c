/*
 * Kanaha Audio - resolver tests (host build: see Makefile in this directory)
 * Licensed under the Apache License, Version 2.0
 *
 * Every case is a sequence of turns in one session. Transcripts marked
 * "spoken" are verbatim whisper output from the rehearsals of 2026-09-21/22
 * (transcripts.jsonl), including the mishearings; the rest cover the rules
 * the orchestrator prompt states but nobody happened to say.
 *
 * Where a case disagrees with what the model did on the day, the comment says
 * so: those are the places the rules were written after the fact.
 */

#include "../../kanaha-audio-app/app/src/main/cpp/voice/kanaha_resolver.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;

/* ---- fixtures: the demo book and the calcs phone's catalog (2026-09-22) ---- */

static kr_book_t BOOKS[1];
static kr_file_t FILES[5];
static kr_context_t CTX;

static void add_file(int i, const char *name, const char *tickers)
{
    char tmp[512], *p, *save = NULL;
    snprintf(FILES[i].name, sizeof(FILES[i].name), "%s", name);
    snprintf(tmp, sizeof(tmp), "%s", tickers);
    FILES[i].n_tickers = 0;
    for (p = strtok_r(tmp, " ", &save); p; p = strtok_r(NULL, " ", &save))
        snprintf(FILES[i].tickers[FILES[i].n_tickers++], KR_TICKER_LEN, "%s", p);
}

static void fixtures(void)
{
    static const char *spoken[] = { "the book", "our book", "the usual five", "the demo book",
                                    "the usual book" };
    static const char *tickers[] = { "MSFT", "AAPL", "AMZN", "JPM", "JNJ" };
    int i;
    kr_book_t *b = &BOOKS[0];
    memset(b, 0, sizeof(*b));
    snprintf(b->name, sizeof(b->name), "demo5");
    for (i = 0; i < 5; i++) snprintf(b->spoken[i], KR_NAME_LEN, "%s", spoken[i]);
    b->n_spoken = 5;
    b->n_assets = 5;
    for (i = 0; i < 5; i++) {
        snprintf(b->tickers[i], KR_TICKER_LEN, "%s", tickers[i]);
        b->weights_pct[i] = 20;
    }
    snprintf(b->file, sizeof(b->file), "fis_daily_closes.csv");
    b->window_years = 10;

    add_file(0, "demo_returns.csv", "");
    add_file(1, "fed_h10_fx_10yr.csv", "DTWEXBGS AUDUSD EURUSD GBPUSD USDJPY");
    add_file(2, "wiki_daily_closes_10yr.csv", "MSFT AAPL AMZN JPM JNJ");
    add_file(3, "fis_daily_closes.csv",
             "MSFT AAPL AMZN JPM JNJ SPY ACWI UUP EURUSD USDJPY GBPUSD AUDUSD");
    add_file(4, "acwi_monthly_returns.csv", "");

    CTX.books = BOOKS;
    CTX.n_books = 1;
    CTX.files = FILES;
    CTX.n_files = 5;
}

/* ---- the harness ---- */

static const char *OUT[] = { "SILENT", "ASK", "REFUSE", "RUN" };

typedef struct {
    const char *said;
    kr_outcome_t expect;
    const char *say_has;    /* substring the spoken line must contain, or NULL */
} turn_t;

static kr_session_t S;
static kr_result_t R;

static void check(int ok, const char *label, const char *detail)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n        %s\n", label, detail ? detail : "");
    }
}

/* Run the turns in one session; R holds the last result. */
static void run(const char *label, const turn_t *turns, int n)
{
    int i;
    char buf[1024];
    kr_session_init(&S);
    for (i = 0; i < n; i++) {
        kr_resolve(&CTX, &S, turns[i].said, &R);
        snprintf(buf, sizeof(buf), "turn %d \"%s\" -> %s \"%s\" (wanted %s%s%s)", i + 1,
                 turns[i].said, OUT[R.outcome], R.say, OUT[turns[i].expect],
                 turns[i].say_has ? " with " : "", turns[i].say_has ? turns[i].say_has : "");
        check(R.outcome == turns[i].expect &&
              (!turns[i].say_has || strstr(R.say, turns[i].say_has)), label, buf);
    }
    printf("  %-58s %s\n", label, OUT[R.outcome]);
}

#define RUN1(label, said, expect, has) do { \
        turn_t t_[] = { { said, expect, has } }; run(label, t_, 1); } while (0)

static int tickers_are(const char *want)
{
    char got[128] = "";
    int i;
    for (i = 0; i < R.spec.n_assets; i++) {
        if (i) strcat(got, " ");
        strcat(got, R.spec.tickers[i]);
    }
    return strcmp(got, want) == 0;
}

static void expect_spec(const char *label, kr_op_t op, kr_regime_t regime, const char *tickers,
                        const char *file, int max_obs)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "op %d regime %d n %d file %s max_obs %d", R.spec.op,
             R.spec.regime, R.spec.n_assets, R.spec.file ? R.spec.file->name : "(none)",
             R.spec.max_obs);
    check(R.outcome == KR_RUN && R.spec.op == op && R.spec.regime == regime &&
          tickers_are(tickers) &&
          (file ? (R.spec.file && strcmp(R.spec.file->name, file) == 0) : !R.spec.file) &&
          R.spec.max_obs == max_obs, label, buf);
}

#define FIVE "MSFT AAPL AMZN JPM JNJ"
#define FIS  "fis_daily_closes.csv"

int main(void)
{
    fixtures();

    printf("route A: historical\n");

    /* spoken 2026-09-22 */
    RUN1("portfolio variance on the book", "portfolio variance on the book", KR_RUN,
         "The book \xe2\x80\x94 MSFT, AAPL, AMZN, JPM, JNJ, equal weights, ten years of the FIS file");
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 2520);
    check(fabs(R.spec.weights[0] - 0.2) < 1e-12 && R.spec.weights_equal, "  equal weights", "");
    check(strstr(R.say, "Portfolio variance.") != NULL, "  read-back names the operation", R.say);

    /* spoken: whisper heard "variants" */
    RUN1("\"Portfolio variants on the book.\"", "Portfolio variants on the book.", KR_RUN, NULL);
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 2520);

    /* spoken. The model ran this; v2 of the flow doc settled that a book names
     * a portfolio, not an operation, so it earns one question. */
    RUN1("\"The Book\" alone asks for the operation", "The Book", KR_ASK,
         "I didn't catch what to run on it");
    /* spoken 2026-09-25 through the Moto, in the Act 2 rehearsal */
    RUN1("\"Portfolio Variates and the Book.\"", "Portfolio Variates and the Book.", KR_RUN,
         "Portfolio variance.");
    /* spoken 2026-09-22 through the Moto: "the book" arrived as "the port." */
    RUN1("\"the port.\" is the book", "the port.", KR_ASK, "The book \xe2\x80\x94 MSFT");
    RUN1("\"variance on the port\"", "portfolio variance on the port", KR_RUN, "The book");
    {
        turn_t t[] = { { "The Book", KR_ASK, NULL },
                       { "portfolio variance", KR_RUN, "Portfolio variance." } };
        run("  ... and \"portfolio variance\" answers it", t, 2);
    }

    /* spoken 2026-09-21: names plus a file named in the utterance */
    RUN1("five names, file named", "portfolio variance for Microsoft, Apple, Amazon, JPMorgan "
         "and Johnson and Johnson, equal weights, all ten years of daily data in "
         "fis_daily_closes.csv", KR_RUN, "Five names, MSFT, AAPL, AMZN, JPM, JNJ");
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 2520);

    /* spoken 2026-09-21. The model picked the wiki file silently and answered
     * 23.39 percent; the rule written afterwards makes it a question. */
    RUN1("five names, \"the file on the phone\" asks which file",
         "portfolio variance for Microsoft, Apple, Amazon, JPMorgan and Johnson and Johnson, "
         "equal weights, ten years of daily data from the file on the phone", KR_ASK,
         "Two files on the phone carry those names: the wiki file and the FIS file. Which one?");

    /* spoken 2026-09-22, two turns */
    {
        turn_t t[] = {
            { "Portfolio variance for Microsoft Apple Amazon JP Morgan and Johnson and Johnson. "
              "Equal weights 10 years.", KR_ASK, "Which one?" },
            { "The FIS daily closes file.", KR_RUN, "ten years of the FIS file" },
        };
        run("names, then \"The FIS daily closes file.\"", t, 2);
        expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 2520);
    }
    {
        turn_t t[] = {
            { "[BLANK_AUDIO] Portfolio variance for Microsoft, Apple, Amazon, JP Morgan, and "
              "Johnson & Johnson.  Equal weights 10 years of daily data from", KR_ASK, NULL },
            { "[BLANK_AUDIO] Use the FIS daily closes file", KR_RUN, NULL },
        };
        run("clipped \"...from\", then \"Use the FIS...\"", t, 2);
        expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 2520);
    }
    {
        turn_t t[] = {
            { "[BLANK_AUDIO] Portfolio variance for Microsoft, Apple, Amazon, JP Morgan, and "
              "Johnson & Johnson.  Equal weights 10 years old.", KR_ASK, NULL },
            { "the wiki one", KR_RUN, "ten years of the wiki file" },
        };
        run("\"10 years old\", then the wiki file", t, 2);
        expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE,
                    "wiki_daily_closes_10yr.csv", 2520);
    }

    /* spoken 2026-09-22: Apple fell out of the clip; four names is still a request */
    {
        turn_t t[] = {
            { "Portfolio variance for Microsoft Amazon, JP Morgan, and Johnson & Johnson.  "
              "Equal weights, 10 years of daily data", KR_ASK, "Which one?" },
            { "fis", KR_RUN, "Four names, MSFT, AMZN, JPM, JNJ" },
        };
        run("four names (no Apple)", t, 2);
        expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, "MSFT AMZN JPM JNJ", FIS,
                    2520);
        check(fabs(R.spec.weights[0] - 0.25) < 1e-12, "  equal weights of 25", "");
    }

    /* spoken 2026-09-22 */
    RUN1("names and file in one sentence", "Portfolio variance for Microsoft Apple Amazon JP "
         "Morgan and Johnson and Johnson. Equal weights 10 years of daily data from the FIS "
         "daily closes file.", KR_RUN, NULL);
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 2520);

    RUN1("the last year", "portfolio variance on the book over the last year", KR_RUN,
         "the last year of the FIS file");
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 252);
    RUN1("twelve months", "variance on the book, last twelve months", KR_RUN, NULL);
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 252);
    RUN1("three years", "the book's variance over three years", KR_RUN, "three years");
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HISTORICAL, FIVE, FIS, 756);

    RUN1("J.P. Morgan with initials", "variance for Microsoft and J.P. Morgan from the FIS file",
         KR_RUN, "Two names, MSFT, JPM");
    RUN1("J and J", "variance for Apple and J and J, fis file", KR_RUN, "AAPL, JNJ");

    printf("route B: a chosen regime\n");

    /* spoken 2026-09-22 */
    RUN1("the book at correlation point eight", "the book at correlation point eight", KR_RUN,
         "Every correlation set to 0.80, historical vols kept: a hypothetical regime. "
         "Portfolio variance.");
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_STRESSED, FIVE, FIS, 2520);
    check(fabs(R.spec.rho - 0.8) < 1e-12, "  rho 0.8", "");

    /* spoken 2026-09-21 */
    RUN1("same book, correlations at point eight", "same book, correlations at point eight",
         KR_RUN, NULL);
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_STRESSED, FIVE, FIS, 2520);

    /* spoken 2026-09-22. The model asked for the previous run's vols; with a
     * book, the resolver has the file and needs no memory of a previous turn. */
    RUN1("\"Same book, but put every correlation at 0.8\"",
         "[BLANK_AUDIO] Same book, but put every correlation at 0.8", KR_RUN, NULL);
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_STRESSED, FIVE, FIS, 2520);

    /* spoken 2026-09-22: "vowel" for vol, "correlation.5" glued, no subject */
    {
        turn_t t[] = {
            { "[BLANK_AUDIO] Every vowel at 22% correlation.5", KR_ASK,
              "On the book \xe2\x80\x94 MSFT, AAPL, AMZN, JPM, JNJ \xe2\x80\x94 or on other names?" },
            { "[BLANK_AUDIO] Yes, the usual five", KR_RUN,
              "Every vol at 22 percent, every correlation 0.50: a hypothetical regime, no file" },
        };
        run("\"Every vowel at 22% correlation.5\", then the book", t, 2);
        expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HYPOTHETICAL, FIVE, NULL, 0);
        check(fabs(R.spec.vol - 0.22) < 1e-12 && fabs(R.spec.rho - 0.5) < 1e-12,
              "  vol 0.22, rho 0.5", "");
    }
    /* the presenter's own word for it, 2026-09-22 */
    RUN1("every volume at twenty-two percent", "every volume at twenty-two percent, correlation "
         "point five, on the book", KR_RUN, "Every vol at 22 percent, every correlation 0.50");
    /* spoken through the Moto, 2026-09-22 */
    RUN1("\"valve\" for vol", "Every valve at 22% correlation point five on the book", KR_RUN,
         "Every vol at 22 percent");
    RUN1("every vol twenty-two percent, correlation point five, the book",
         "every vol at twenty-two percent, correlation point five, on the book", KR_RUN, NULL);
    expect_spec("  spec", KR_OP_VARIANCE, KR_REGIME_HYPOTHETICAL, FIVE, NULL, 0);
    {
        turn_t t[] = { { "the book with every vol at 30 percent", KR_ASK, "what correlation" },
                       { "point three", KR_ASK, NULL } };
        /* "point three" alone has no correlation keyword: not an answer. */
        run("vol without a correlation asks for it", t, 1);
    }
    {
        turn_t t[] = { { "the book with every vol at 30 percent", KR_ASK, "what correlation" },
                       { "correlation point three", KR_RUN, "every correlation 0.30" } };
        run("  ... and \"correlation point three\" answers it", t, 2);
    }
    RUN1("negative correlation", "the book at correlation minus point two", KR_RUN, "-0.20");
    RUN1("zero point eight", "the book at a correlation of zero point eight", KR_RUN, "0.80");

    printf("simulation\n");

    /* spoken 2026-09-22 */
    RUN1("simulate the book forward a year", "Simulate the book forward a year.", KR_RUN,
         "Simulating one year forward, ten thousand paths, seed 12345.");
    expect_spec("  spec (\"a year\" is the horizon, not the window)", KR_OP_SIMULATE,
                KR_REGIME_HISTORICAL, FIVE, FIS, 2520);
    check(R.spec.seed == 12345 && R.spec.n_simulations == 10000, "  seed and paths", "");

    /* spoken 2026-09-22, arrived damaged: "simulate it" was lost, "forward" survived */
    RUN1("\"Same book at correlation.8,  forward\"", "Same book at correlation.8,  forward",
         KR_RUN, "Simulating");
    expect_spec("  spec", KR_OP_SIMULATE, KR_REGIME_STRESSED, FIVE, FIS, 2520);

    RUN1("value at risk", "value at risk on the book", KR_RUN, "Simulating");

    /* No verb is carried across turns: after a simulation, a bare regime is a
     * variance, and the read-back says so. */
    {
        turn_t t[] = { { "simulate the book forward", KR_RUN, "Simulating" },
                       { "same book at correlation point eight", KR_RUN, "Portfolio variance." } };
        run("a verb is never inherited from the previous turn", t, 2);
    }

    printf("refusals\n");

    /* spoken 2026-09-22 */
    RUN1("three decimals", "[BLANK_AUDIO] [BEEP]  Same book.  Correlations at 0.857", KR_REFUSE,
         "I heard a correlation of 0.857, three decimals");
    RUN1("a covariance cell, misheard", "Set the whole variance between apple and amazon to 0.042",
         KR_REFUSE, "I won't take a covariance by voice");
    RUN1("a covariance cell", "Set the co-variance between Apple and Amazon to 0.042", KR_REFUSE,
         "I won't take a covariance by voice");
    RUN1("correlation out of range", "the book at correlation 1.4", KR_REFUSE,
         "I heard a correlation of 1.4. Correlations run from minus one to one.");
    RUN1("weights that do not sum", "variance on the book, weights 30 30 30 20 20", KR_REFUSE,
         "Those weights sum to 130 percent");
    RUN1("weights that sum within one", "variance on the book, weights 20 20 20 20 19", KR_RUN,
         "normalised to 100");
    RUN1("weights twenty-five, twenty-five, twenty, fifteen, fifteen",
         "portfolio variance on the book, weights twenty five twenty five twenty fifteen fifteen",
         KR_RUN, "weights 25, 25, 20, 15, 15 percent");
    check(fabs(R.spec.weights[0] - 0.25) < 1e-12 && fabs(R.spec.weights[4] - 0.15) < 1e-12,
          "  weights as fractions", "");
    RUN1("wrong number of weights", "variance on the book, weights 50 50", KR_REFUSE,
         "I heard 2 weights for 5 names");
    /* spoken 2026-09-22: the start of the sentence was lost */
    RUN1("one name is not a portfolio", "[BLANK_AUDIO] Johnson & Johnson equal weights 10 years "
         "of daily data from the FIS daily closes  file with every correlation at 0.8", KR_REFUSE,
         "I only heard one name, JNJ");
    RUN1("a vol in decimals", "the book with every vol at 0.22 and correlation 0.5", KR_REFUSE,
         "whole percent");
    RUN1("the book on another file", "variance on the book from the wiki file", KR_REFUSE,
         "The book is defined on the FIS file");
    RUN1("a name it cannot map is not guessed", "variance for Microsoft and USDJPY", KR_REFUSE,
         "I only heard one name, MSFT");

    printf("silence\n");

    /* spoken 2026-09-22 */
    RUN1("a blank clip", "[BLANK_AUDIO] [BEEP]", KR_SILENT, NULL);
    RUN1("room chatter", "okay so as I was saying about the weather", KR_SILENT, NULL);
    /* spoken 2026-09-22, when nothing was pending */
    RUN1("a file on its own", "The FIS daily closes file.", KR_SILENT, NULL);
    {
        turn_t t[] = {
            { "Portfolio variance for Microsoft Apple Amazon JP Morgan and Johnson and Johnson.",
              KR_ASK, "Which one?" },
            { "[BLANK_AUDIO]", KR_SILENT, NULL },
            { "the FIS one", KR_RUN, NULL },
        };
        run("a blank clip keeps the pending question", t, 3);
    }
    {
        turn_t t[] = {
            { "Portfolio variance for Microsoft Apple Amazon JP Morgan and Johnson and Johnson.",
              KR_ASK, "Which one?" },
            { "simulate the book forward", KR_RUN, "Simulating" },
        };
        run("a new full request replaces the pending question", t, 2);
    }

    printf("%s: %d of %d checks failed\n", failures ? "FAILED" : "PASSED", failures, checks);
    return failures ? 1 : 0;
}
