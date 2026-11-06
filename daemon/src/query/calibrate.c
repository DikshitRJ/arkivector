/*
 * query/calibrate.c: Startup calibration of distance calls and HNSW searches (c_d and rho).
 */
#include "query/calibrate.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server/metrics.h"

#include "postgres.h"
#include "varatt.h"
#include "vector.h"

static pthread_mutex_t g_calib_mu = PTHREAD_MUTEX_INITIALIZER;
static av_calibration g_calib = {
    .c_d_ns = 150.0,
    .rho = 2.89,
    .gamma = 1.7
};

static double time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e9 + (double) ts.tv_nsec;
}

const av_calibration *av_calibration_get(void)
{
    pthread_mutex_lock(&g_calib_mu);
    const av_calibration *c = &g_calib;
    pthread_mutex_unlock(&g_calib_mu);
    return c;
}

void av_calibration_set(const av_calibration *c)
{
    if (c == NULL)
        return;
    pthread_mutex_lock(&g_calib_mu);
    g_calib = *c;
    if (g_calib.rho <= 0.0)
        g_calib.rho = 2.89;
    g_calib.gamma = sqrt(g_calib.rho);
    pthread_mutex_unlock(&g_calib_mu);
}

av_status av_calibrate_startup(av_calibration *out, av_err *e)
{
    (void) e;
    const int dim = 128;
    const size_t n_calls = 10000;

    Vector *v1 = malloc(VECTOR_SIZE((size_t) dim));
    Vector *v2 = malloc(VECTOR_SIZE((size_t) dim));
    if (v1 == NULL || v2 == NULL) {
        free(v1);
        free(v2);
        if (out != NULL)
            *out = *av_calibration_get();
        return AV_OK;
    }

    SET_VARSIZE(v1, (uint32) VECTOR_SIZE((size_t) dim));
    v1->dim = (int16) dim;
    v1->unused = 0;
    SET_VARSIZE(v2, (uint32) VECTOR_SIZE((size_t) dim));
    v2->dim = (int16) dim;
    v2->unused = 0;

    for (int i = 0; i < dim; i++) {
        v1->x[i] = (float) i * 0.01f;
        v2->x[i] = (float) i * 0.02f;
    }

    const av_opclass *oc = av_opclass_find("vector_l2_ops");
    if (oc == NULL || oc->distance == NULL) {
        free(v1);
        free(v2);
        if (out != NULL)
            *out = *av_calibration_get();
        return AV_OK;
    }

    /* Warm-up */
    volatile double dummy = 0.0;
    for (size_t i = 0; i < 100; i++) {
        dummy += oc->distance(v1, v2);
    }

    double t0 = time_now_ns();
    for (size_t i = 0; i < n_calls; i++) {
        dummy += oc->distance(v1, v2);
    }
    double t1 = time_now_ns();

    (void) dummy;
    free(v1);
    free(v2);

    double elapsed = t1 - t0;
    double cd = elapsed / (double) n_calls;
    if (cd < 5.0) cd = 5.0;
    if (cd > 5000.0) cd = 5000.0;

    pthread_mutex_lock(&g_calib_mu);
    g_calib.c_d_ns = cd;
    g_calib.rho = 2.89;
    g_calib.gamma = 1.7;
    if (out != NULL)
        *out = g_calib;
    pthread_mutex_unlock(&g_calib_mu);

    return AV_OK;
}

void av_calibration_record_flat(double elapsed_ns, size_t count)
{
    if (count == 0 || elapsed_ns <= 0.0)
        return;
    double sample_cd = elapsed_ns / (double) count;
    pthread_mutex_lock(&g_calib_mu);
    double alpha = 0.05;
    g_calib.c_d_ns = (1.0 - alpha) * g_calib.c_d_ns + alpha * sample_cd;
    pthread_mutex_unlock(&g_calib_mu);
}

void av_calibration_record_hnsw(double elapsed_ns, size_t count)
{
    if (count == 0 || elapsed_ns <= 0.0)
        return;
    pthread_mutex_lock(&g_calib_mu);
    double cost_per_node = elapsed_ns / (double) count;
    double sample_rho = g_calib.c_d_ns > 0 ? (cost_per_node / g_calib.c_d_ns) : 2.89;
    if (sample_rho >= 1.0 && sample_rho <= 10.0) {
        double alpha = 0.05;
        g_calib.rho = (1.0 - alpha) * g_calib.rho + alpha * sample_rho;
        g_calib.gamma = sqrt(g_calib.rho);
    }
    pthread_mutex_unlock(&g_calib_mu);
}

static av_mfamily *g_metric_recall = NULL;
static pthread_once_t g_recall_once = PTHREAD_ONCE_INIT;

static void init_recall_metric(void)
{
    static const char *const labels[] = {"collection"};
    g_metric_recall = av_metric_register("arkivector_recall_at_k",
                                         "Recall@k canary metric for approximate search",
                                         AV_MTYPE_GAUGE, labels, 1);
}

void av_recall_canary_record(const char *collection, double recall)
{
    pthread_once(&g_recall_once, init_recall_metric);
    if (g_metric_recall != NULL && collection != NULL) {
        const char *label_vals[] = {collection};
        av_metric_set(g_metric_recall, label_vals, recall);
    }
}
