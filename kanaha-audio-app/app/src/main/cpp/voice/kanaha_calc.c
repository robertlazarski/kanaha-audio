/*
 * Kanaha Audio - calls to Kanaha Calcs for a resolved request
 * Licensed under the Apache License, Version 2.0
 */

#include "kanaha_calc.h"

#include <json-c/json.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double num(json_object *o, const char *key)
{
    json_object *v;
    return json_object_object_get_ex(o, key, &v) ? json_object_get_double(v) : 0.0;
}

static const char *str(json_object *o, const char *key)
{
    json_object *v;
    return (json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string))
        ? json_object_get_string(v) : NULL;
}

static json_object *doubles(const double *v, int n)
{
    json_object *a = json_object_new_array();
    int i;
    for (i = 0; i < n; i++)
        json_object_array_add(a, json_object_new_double(v[i]));
    return a;
}

/* POST one operation; the parsed response, or NULL with out->error set.
 * Takes ownership of `req`. A service refusal is quoted as it came. */
static json_object *call(axis2_h2_json_client_t *client, const axutil_env_t *env,
                         const char *op, json_object *req, kc_result_t *out)
{
    char path[128];
    const char *body = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
    axis2_char_t *resp = NULL;
    size_t len = 0;
    int status = 0;
    json_object *r;

    snprintf(path, sizeof(path), KC_SERVICE_PATH "%s", op);
    if (axis2_h2_json_client_post(client, env, path, body, strlen(body), &resp, &len,
                                  &status) != AXIS2_SUCCESS) {
        snprintf(out->error, sizeof(out->error), "I couldn't reach the calcs phone: %s.",
                 axis2_h2_json_client_get_error(client));
        json_object_put(req);
        return NULL;
    }
    json_object_put(req);
    r = json_tokener_parse(resp);
    AXIS2_FREE(env->allocator, resp);
    if (!r) {
        snprintf(out->error, sizeof(out->error), "The calcs phone answered %s with something "
                 "that is not JSON (HTTP %d).", op, status);
        return NULL;
    }
    if (!str(r, "status") || strcmp(str(r, "status"), "SUCCESS") != 0) {
        const char *why = str(r, "error_message");
        snprintf(out->error, sizeof(out->error), "The calcs phone refused %s: %s", op,
                 why ? why : "no reason given.");
        json_object_put(r);
        return NULL;
    }
    if (out->tools[0]) strncat(out->tools, ",", sizeof(out->tools) - strlen(out->tools) - 1);
    strncat(out->tools, op, sizeof(out->tools) - strlen(out->tools) - 1);
    return r;
}

/* The response's covariance_matrix into m (n*n); 0 on a wrong shape. */
static int take_matrix(json_object *r, int n, double *m)
{
    json_object *a;
    int i;
    if (!json_object_object_get_ex(r, "covariance_matrix", &a) ||
        !json_object_is_type(a, json_type_array) ||
        (int)json_object_array_length(a) != n * n)
        return 0;
    for (i = 0; i < n * n; i++)
        m[i] = json_object_get_double(json_object_array_get_idx(a, (size_t)i));
    return 1;
}

static int take_vols(json_object *r, int n, double *vols)
{
    json_object *a;
    int i;
    if (!json_object_object_get_ex(r, "volatilities", &a) ||
        !json_object_is_type(a, json_type_array) || (int)json_object_array_length(a) != n)
        return 0;
    for (i = 0; i < n; i++)
        vols[i] = json_object_get_double(json_object_array_get_idx(a, (size_t)i));
    return 1;
}

void kc_execute(axis2_h2_json_client_t *client, const axutil_env_t *env,
                const kr_spec_t *sp, kc_result_t *out)
{
    double m[KR_MAX_ASSETS * KR_MAX_ASSETS];
    int n = sp->n_assets, i;
    long t0 = now_ms();
    json_object *req, *r;

    memset(out, 0, sizeof(*out));
    snprintf(out->request_id, sizeof(out->request_id), "kv-%s-%s-%s",
             sp->op == KR_OP_SIMULATE ? "mc" : "pv",
             sp->regime == KR_REGIME_HISTORICAL ? "hist" :
             sp->regime == KR_REGIME_STRESSED ? "stress" : "hypo",
             sp->book ? sp->book->name : "names");

    /* 1. The matrix: from the file, or chosen. */
    if (sp->regime != KR_REGIME_HYPOTHETICAL) {
        json_object *cols = json_object_new_array();
        const char *d;
        for (i = 0; i < n; i++) {
            char col[KR_TICKER_LEN + 16];
            snprintf(col, sizeof(col), "%s_AdjClose", sp->tickers[i]);
            json_object_array_add(cols, json_object_new_string(col));
        }
        req = json_object_new_object();
        json_object_object_add(req, "file", json_object_new_string(sp->file->name));
        json_object_object_add(req, "columns", cols);
        if (sp->max_obs > 0)
            json_object_object_add(req, "max_obs", json_object_new_int(sp->max_obs));
        json_object_object_add(req, "request_id", json_object_new_string(out->request_id));
        if (!(r = call(client, env, "covarianceFromCsv", req, out)))
            goto done;
        if (!take_matrix(r, n, m) || !take_vols(r, n, out->vols)) {
            snprintf(out->error, sizeof(out->error), "The calcs phone's covariance matrix did "
                     "not have %d assets.", n);
            json_object_put(r);
            goto done;
        }
        if ((d = str(r, "first_row_date"))) snprintf(out->first_date, sizeof(out->first_date), "%s", d);
        if ((d = str(r, "last_row_date"))) snprintf(out->last_date, sizeof(out->last_date), "%s", d);
        out->n_obs = (int)num(r, "n_obs_used");
        json_object_put(r);
    } else {
        for (i = 0; i < n; i++) out->vols[i] = sp->vol;
    }

    if (sp->regime != KR_REGIME_HISTORICAL) {
        req = json_object_new_object();
        json_object_object_add(req, "volatilities", doubles(out->vols, n));
        json_object_object_add(req, "correlation", json_object_new_double(sp->rho));
        json_object_object_add(req, "request_id", json_object_new_string(out->request_id));
        if (!(r = call(client, env, "composeCovariance", req, out)))
            goto done;
        if (!take_matrix(r, n, m)) {
            snprintf(out->error, sizeof(out->error), "The composed matrix did not have %d "
                     "assets.", n);
            json_object_put(r);
            goto done;
        }
        json_object_put(r);
    }
    for (i = 0; i < n; i++) out->sigma_trace += m[i * n + i];

    /* 2. The operation. */
    req = json_object_new_object();
    json_object_object_add(req, "n_assets", json_object_new_int(n));
    json_object_object_add(req, "weights", doubles(sp->weights, n));
    json_object_object_add(req, "covariance_matrix", doubles(m, n * n));
    json_object_object_add(req, "request_id", json_object_new_string(out->request_id));
    if (sp->op == KR_OP_SIMULATE) {
        json_object_object_add(req, "n_simulations", json_object_new_int(sp->n_simulations));
        json_object_object_add(req, "random_seed", json_object_new_int(sp->seed));
        json_object_object_add(req, "n_periods", json_object_new_int(252));
        json_object_object_add(req, "n_periods_per_year", json_object_new_int(252));
        json_object_object_add(req, "initial_value", json_object_new_double(KC_INITIAL_VALUE));
        if (!(r = call(client, env, "monteCarlo", req, out)))
            goto done;
        out->initial_value = KC_INITIAL_VALUE;
        out->var_95 = num(r, "var_95");
        out->var_99 = num(r, "var_99");
        out->cvar_95 = num(r, "cvar_95");
        out->max_drawdown = num(r, "max_drawdown");
        out->prob_profit = num(r, "prob_profit");
        out->volatility = num(r, "portfolio_volatility");
        out->variance = out->volatility * out->volatility;
    } else {
        /* The matrix is annualised whichever route built it. */
        json_object_object_add(req, "n_periods_per_year", json_object_new_int(1));
        if (!(r = call(client, env, "portfolioVariance", req, out)))
            goto done;
        out->variance = num(r, "portfolio_variance");
        out->volatility = num(r, "portfolio_volatility");
    }
    out->calc_time_us = (long)num(r, "calc_time_us");
    json_object_put(r);
    out->ok = 1;
done:
    out->round_trip_ms = now_ms() - t0;
}

/* ------------------------------------------------------------------------ */
/* loaders                                                                   */
/* ------------------------------------------------------------------------ */

int kc_load_books(const char *path, kr_book_t *books, int max, char *err, int err_len)
{
    json_object *root = json_object_from_file(path), *all;
    int n = 0;
    if (!root) {
        snprintf(err, (size_t)err_len, "cannot read books from %s", path);
        return -1;
    }
    if (!json_object_object_get_ex(root, "books", &all) ||
        !json_object_is_type(all, json_type_object)) {
        snprintf(err, (size_t)err_len, "%s has no \"books\" object", path);
        json_object_put(root);
        return -1;
    }
    json_object_object_foreach(all, name, b) {
        kr_book_t *k;
        json_object *a;
        int i;
        if (n >= max) break;
        k = &books[n];
        memset(k, 0, sizeof(*k));
        snprintf(k->name, sizeof(k->name), "%s", name);
        if (json_object_object_get_ex(b, "spoken_as", &a) && json_object_is_type(a, json_type_array))
            for (i = 0; i < (int)json_object_array_length(a) && i < KR_MAX_SPOKEN; i++)
                snprintf(k->spoken[k->n_spoken++], KR_NAME_LEN, "%s",
                         json_object_get_string(json_object_array_get_idx(a, (size_t)i)));
        if (json_object_object_get_ex(b, "assets", &a) && json_object_is_type(a, json_type_array))
            for (i = 0; i < (int)json_object_array_length(a) && i < KR_MAX_ASSETS; i++)
                snprintf(k->tickers[k->n_assets++], KR_TICKER_LEN, "%s",
                         json_object_get_string(json_object_array_get_idx(a, (size_t)i)));
        if (json_object_object_get_ex(b, "weights_percent", &a) &&
            json_object_is_type(a, json_type_array))
            for (i = 0; i < (int)json_object_array_length(a) && i < k->n_assets; i++)
                k->weights_pct[i] = json_object_get_double(json_object_array_get_idx(a, (size_t)i));
        if (str(b, "source_file")) snprintf(k->file, sizeof(k->file), "%s", str(b, "source_file"));
        k->window_years = (int)num(b, "window_years");
        if (k->n_assets < 2 || !k->file[0] || k->n_spoken == 0) {
            snprintf(err, (size_t)err_len, "book %s needs at least two assets, a source_file "
                     "and a spoken_as list", name);
            json_object_put(root);
            return -1;
        }
        n++;
    }
    json_object_put(root);
    return n;
}

int kc_fetch_catalog(axis2_h2_json_client_t *client, const axutil_env_t *env,
                     kr_file_t *files, int max, char *err, int err_len)
{
    kc_result_t tmp;
    json_object *r, *list;
    int n = 0, i, j;

    memset(&tmp, 0, sizeof(tmp));
    if (!(r = call(client, env, "listCsvFiles", json_object_new_object(), &tmp))) {
        snprintf(err, (size_t)err_len, "%s", tmp.error);
        return -1;
    }
    if (!json_object_object_get_ex(r, "files", &list) || !json_object_is_type(list, json_type_array)) {
        snprintf(err, (size_t)err_len, "listCsvFiles returned no files array");
        json_object_put(r);
        return -1;
    }
    for (i = 0; i < (int)json_object_array_length(list) && n < max; i++) {
        json_object *f = json_object_array_get_idx(list, (size_t)i), *cols;
        kr_file_t *k = &files[n];
        memset(k, 0, sizeof(*k));
        if (!str(f, "file")) continue;
        snprintf(k->name, sizeof(k->name), "%s", str(f, "file"));
        if (json_object_object_get_ex(f, "columns", &cols) && json_object_is_type(cols, json_type_array))
            for (j = 0; j < (int)json_object_array_length(cols) && k->n_tickers < KR_MAX_COLUMNS; j++) {
                const char *c = json_object_get_string(json_object_array_get_idx(cols, (size_t)j));
                size_t l = c ? strlen(c) : 0;
                if (l > 9 && l - 9 < KR_TICKER_LEN && strcmp(c + l - 9, "_AdjClose") == 0)
                    snprintf(k->tickers[k->n_tickers++], KR_TICKER_LEN, "%.*s", (int)(l - 9), c);
            }
        n++;
    }
    json_object_put(r);
    return n;
}
