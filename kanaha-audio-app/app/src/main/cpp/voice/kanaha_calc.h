/*
 * Kanaha Audio - calls to Kanaha Calcs for a resolved request
 * Licensed under the Apache License, Version 2.0
 *
 * Runs a kr_spec_t against the calcs phone's FinancialBenchmarkService over
 * the Axis2/C HTTP/2 JSON client (axis2_h2_json_client.h), with this phone's
 * own certificate for mTLS. At most three calls per request:
 *
 *   historical    covarianceFromCsv                       -> portfolioVariance | monteCarlo
 *   stressed      covarianceFromCsv -> composeCovariance  -> portfolioVariance | monteCarlo
 *   hypothetical                      composeCovariance   -> portfolioVariance | monteCarlo
 *
 * A stressed request reuses the volatilities an earlier request measured for
 * the same file, window and names (a kc_cache_t kept by the caller), so its
 * route is composeCovariance -> monteCarlo with no file opened: the flow
 * chart's route B (kanaha_calcs_mc_flow.md), where the chosen regime starts
 * from measured vols and the only disk read belongs to route A. With nothing
 * cached yet it reads the file once for the vols.
 *
 * The matrix passed on is always annualised, so portfolioVariance gets
 * n_periods_per_year 1. A refusal from the service is quoted, never retried
 * with a nudged number.
 *
 * Also the two loaders the resolver needs: the books file and the calcs
 * phone's CSV catalog (listCsvFiles).
 */

#ifndef KANAHA_CALC_H
#define KANAHA_CALC_H

#include "kanaha_resolver.h"
#include "kanaha_answer.h"

#include <axutil_env.h>
#include <axis2_h2_json_client.h>

#define KC_SERVICE_PATH "/services/FinancialBenchmarkService/"
#define KC_INITIAL_VALUE 1000000.0

/* What the file measured, for reuse by a stressed request on the same book. */
typedef struct {
    int valid;
    char key[512];                  /* file, window and names */
    double vols[KR_MAX_ASSETS];
    char first_date[16], last_date[16];
    int n_obs;
} kc_cache_t;

/* Run one resolved spec. out->ok is 0 with a sayable out->error on failure.
 * `cache` may be NULL; when given it is read by stressed requests and
 * refreshed by every request that reads the file. */
void kc_execute(axis2_h2_json_client_t *client, const axutil_env_t *env,
                const kr_spec_t *spec, kc_cache_t *cache, kc_result_t *out);

/* Read kanaha-books.json. Returns the number of books, or -1 with err set. */
int kc_load_books(const char *path, kr_book_t *books, int max, char *err, int err_len);

/* Ask the calcs phone which CSVs it holds and which <TICKER>_AdjClose
 * columns each carries. Returns the number of files, or -1 with err set. */
int kc_fetch_catalog(axis2_h2_json_client_t *client, const axutil_env_t *env,
                     kr_file_t *files, int max, char *err, int err_len);

#endif /* KANAHA_CALC_H */
