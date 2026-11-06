/*
 * server/metrics.h: counters, gauges and histograms with labels, Prometheus text exposition.
 *
 * A metric family is registered once by name (idempotent: the same name returns the same
 * family; a different type or label set for an existing name returns NULL). Each distinct
 * label-value tuple is a series. A family holds at most AV_METRICS_MAX_SERIES series; further
 * tuples are dropped and counted in arkivector_metrics_dropped_series_total.
 * Thread safety: everything is thread-safe (one mutex per family, one for the registry).
 * Ownership: the registry owns families; the process-wide registry lives until exit.
 */
#ifndef AV_SERVER_METRICS_H
#define AV_SERVER_METRICS_H

#include <stddef.h>

typedef enum { AV_MTYPE_COUNTER, AV_MTYPE_GAUGE, AV_MTYPE_HISTOGRAM } av_mtype;

typedef struct av_mfamily av_mfamily;

#define AV_METRICS_MAX_SERIES 1024
#define AV_METRICS_MAX_LABELS 4

/* label_names: array of n_labels C strings (may be NULL when n_labels == 0). */
av_mfamily *av_metric_register(const char *name, const char *help, av_mtype type,
                               const char *const *label_names, size_t n_labels);
/* Histogram with explicit ascending upper bounds (no +Inf). */
av_mfamily *av_metric_register_histogram(const char *name, const char *help, const char *const *label_names,
                                         size_t n_labels, const double *bounds, size_t n_bounds);

/* label_values: n_labels strings (NULL when none). NULL metric is a no-op. */
void av_metric_inc(av_mfamily *m, const char *const *label_values, double delta);     /* counter, gauge */
void av_metric_set(av_mfamily *m, const char *const *label_values, double value);     /* gauge */
void av_metric_observe(av_mfamily *m, const char *const *label_values, double value); /* histogram */

/* Render all families in Prometheus text format 0.0.4. Returns malloc'd NUL-terminated text. */
char *av_metrics_render(size_t *len);

/* Value of one series (counter/gauge value, histogram count). Returns 0 and sets *found=false if absent. */
double av_metric_value(av_mfamily *m, const char *const *label_values, int *found);

#endif /* AV_SERVER_METRICS_H */
