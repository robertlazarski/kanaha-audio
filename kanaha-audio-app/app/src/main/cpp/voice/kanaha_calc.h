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

/* Run one resolved spec. out->ok is 0 with a sayable out->error on failure. */
void kc_execute(axis2_h2_json_client_t *client, const axutil_env_t *env,
                const kr_spec_t *spec, kc_result_t *out);

/* Read kanaha-books.json. Returns the number of books, or -1 with err set. */
int kc_load_books(const char *path, kr_book_t *books, int max, char *err, int err_len);

/* Ask the calcs phone which CSVs it holds and which <TICKER>_AdjClose
 * columns each carries. Returns the number of files, or -1 with err set. */
int kc_fetch_catalog(axis2_h2_json_client_t *client, const axutil_env_t *env,
                     kr_file_t *files, int max, char *err, int err_len);

#endif /* KANAHA_CALC_H */
