/*
 * server/metrics.c: metrics registry and Prometheus text exposition. See metrics.h.
 */
#include "server/metrics.h"

#include <math.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *labels[AV_METRICS_MAX_LABELS];
    double value;     /* counter/gauge value, histogram sum */
    uint64_t count;   /* histogram count */
    uint64_t *bucket; /* histogram: per-bound cumulative-on-render counts (non-cumulative here) */
} series;

struct av_mfamily {
    char *name;
    char *help;
    av_mtype type;
    char *label_names[AV_METRICS_MAX_LABELS];
    size_t n_labels;
    double *bounds;
    size_t n_bounds;
    pthread_mutex_t mu;
    series *s;
    size_t n_series;
    size_t cap_series;
    av_mfamily *next;
};

static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static av_mfamily *g_head;
static av_mfamily *g_tail;
static av_mfamily *g_dropped;

static const double k_default_bounds[] = {0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10};

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

static av_mfamily *register_locked(const char *name, const char *help, av_mtype type,
                                   const char *const *label_names, size_t n_labels, const double *bounds,
                                   size_t n_bounds)
{
    for (av_mfamily *m = g_head; m != NULL; m = m->next) {
        if (strcmp(m->name, name) == 0) {
            if (m->type != type || m->n_labels != n_labels) {
                return NULL;
            }
            for (size_t i = 0; i < n_labels; i++) {
                if (strcmp(m->label_names[i], label_names[i]) != 0) {
                    return NULL;
                }
            }
            return m;
        }
    }
    av_mfamily *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    m->name = dup_str(name);
    m->help = dup_str(help != NULL ? help : "");
    m->type = type;
    m->n_labels = n_labels;
    for (size_t i = 0; i < n_labels; i++) {
        m->label_names[i] = dup_str(label_names[i]);
    }
    if (type == AV_MTYPE_HISTOGRAM) {
        m->n_bounds = n_bounds;
        m->bounds = malloc(n_bounds * sizeof(double));
        if (m->bounds == NULL) {
            return NULL;
        }
        memcpy(m->bounds, bounds, n_bounds * sizeof(double));
    }
    pthread_mutex_init(&m->mu, NULL);
    if (g_tail != NULL) {
        g_tail->next = m;
    } else {
        g_head = m;
    }
    g_tail = m;
    return m;
}

static av_mfamily *do_register(const char *name, const char *help, av_mtype type,
                               const char *const *label_names, size_t n_labels, const double *bounds,
                               size_t n_bounds)
{
    if (name == NULL || n_labels > AV_METRICS_MAX_LABELS || (n_labels > 0 && label_names == NULL)) {
        return NULL;
    }
    pthread_mutex_lock(&g_reg_mu);
    av_mfamily *m = register_locked(name, help, type, label_names, n_labels, bounds, n_bounds);
    pthread_mutex_unlock(&g_reg_mu);
    return m;
}

av_mfamily *av_metric_register(const char *name, const char *help, av_mtype type,
                               const char *const *label_names, size_t n_labels)
{
    return do_register(name, help, type, label_names, n_labels, k_default_bounds,
                       sizeof(k_default_bounds) / sizeof(k_default_bounds[0]));
}

av_mfamily *av_metric_register_histogram(const char *name, const char *help, const char *const *label_names,
                                         size_t n_labels, const double *bounds, size_t n_bounds)
{
    if (bounds == NULL || n_bounds == 0) {
        return NULL;
    }
    return do_register(name, help, AV_MTYPE_HISTOGRAM, label_names, n_labels, bounds, n_bounds);
}

static void count_dropped(void)
{
    pthread_mutex_lock(&g_reg_mu);
    if (g_dropped == NULL) {
        g_dropped = register_locked("arkivector_metrics_dropped_series_total",
                                    "Label tuples dropped because a family hit its series cap.",
                                    AV_MTYPE_COUNTER, NULL, 0, NULL, 0);
    }
    av_mfamily *d = g_dropped;
    pthread_mutex_unlock(&g_reg_mu);
    if (d != NULL) {
        av_metric_inc(d, NULL, 1);
    }
}

/* Caller holds m->mu. */
static series *find_series(av_mfamily *m, const char *const *lv, int create)
{
    for (size_t i = 0; i < m->n_series; i++) {
        series *s = &m->s[i];
        size_t k = 0;
        while (k < m->n_labels && strcmp(s->labels[k], lv[k]) == 0) {
            k++;
        }
        if (k == m->n_labels) {
            return s;
        }
    }
    if (!create || m->n_series >= AV_METRICS_MAX_SERIES) {
        return NULL;
    }
    if (m->n_series == m->cap_series) {
        size_t nc = m->cap_series == 0 ? 8 : m->cap_series * 2;
        series *ns = realloc(m->s, nc * sizeof(series));
        if (ns == NULL) {
            return NULL;
        }
        m->s = ns;
        m->cap_series = nc;
    }
    series *s = &m->s[m->n_series];
    memset(s, 0, sizeof(*s));
    for (size_t k = 0; k < m->n_labels; k++) {
        s->labels[k] = dup_str(lv[k]);
        if (s->labels[k] == NULL) {
            return NULL;
        }
    }
    if (m->type == AV_MTYPE_HISTOGRAM) {
        s->bucket = calloc(m->n_bounds + 1, sizeof(uint64_t));
        if (s->bucket == NULL) {
            return NULL;
        }
    }
    m->n_series++;
    return s;
}

static const char *const k_no_labels[1] = {NULL};

static void update(av_mfamily *m, const char *const *lv, int op, double v)
{
    if (m == NULL) {
        return;
    }
    if (lv == NULL) {
        if (m->n_labels != 0) {
            return;
        }
        lv = k_no_labels;
    }
    pthread_mutex_lock(&m->mu);
    series *s = find_series(m, lv, 1);
    if (s == NULL) {
        pthread_mutex_unlock(&m->mu);
        count_dropped();
        return;
    }
    if (op == 0) { /* add */
        s->value += v;
    } else if (op == 1) { /* set */
        s->value = v;
    } else { /* observe */
        size_t b = 0;
        while (b < m->n_bounds && v > m->bounds[b]) {
            b++;
        }
        s->bucket[b]++;
        s->count++;
        s->value += v;
    }
    pthread_mutex_unlock(&m->mu);
}

void av_metric_inc(av_mfamily *m, const char *const *lv, double delta)
{
    if (m != NULL && m->type != AV_MTYPE_HISTOGRAM) {
        update(m, lv, 0, m->type == AV_MTYPE_COUNTER && delta < 0 ? 0 : delta);
    }
}

void av_metric_set(av_mfamily *m, const char *const *lv, double value)
{
    if (m != NULL && m->type == AV_MTYPE_GAUGE) {
        update(m, lv, 1, value);
    }
}

void av_metric_observe(av_mfamily *m, const char *const *lv, double value)
{
    if (m != NULL && m->type == AV_MTYPE_HISTOGRAM) {
        update(m, lv, 2, value);
    }
}

double av_metric_value(av_mfamily *m, const char *const *lv, int *found)
{
    double out = 0;
    int ok = 0;
    if (m != NULL) {
        pthread_mutex_lock(&m->mu);
        series *s = find_series(m, lv != NULL ? lv : k_no_labels, 0);
        if (s != NULL) {
            ok = 1;
            out = m->type == AV_MTYPE_HISTOGRAM ? (double) s->count : s->value;
        }
        pthread_mutex_unlock(&m->mu);
    }
    if (found != NULL) {
        *found = ok;
    }
    return out;
}

/* ---- rendering ---- */

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->p == NULL && b->cap == (size_t) -1) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap == 0 ? 4096 : b->cap;
        while (nc < b->len + n + 1) {
            nc *= 2;
        }
        char *np = realloc(b->p, nc);
        if (np == NULL) {
            free(b->p);
            b->p = NULL;
            b->cap = (size_t) -1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_str(sbuf *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

static void sb_num(sbuf *b, double v)
{
    char t[40];
    int n;
    if (isinf(v)) {
        n = snprintf(t, sizeof(t), "%s", v > 0 ? "+Inf" : "-Inf");
    } else if (isnan(v)) {
        n = snprintf(t, sizeof(t), "NaN");
    } else if (v == floor(v) && fabs(v) < 1e15) {
        n = snprintf(t, sizeof(t), "%.0f", v);
    } else {
        n = snprintf(t, sizeof(t), "%.9g", v);
    }
    sb_put(b, t, (size_t) n);
}

static void sb_escaped(sbuf *b, const char *s, int escape_quote)
{
    for (; *s != '\0'; s++) {
        if (*s == '\\') {
            sb_str(b, "\\\\");
        } else if (*s == '\n') {
            sb_str(b, "\\n");
        } else if (*s == '"' && escape_quote) {
            sb_str(b, "\\\"");
        } else {
            sb_put(b, s, 1);
        }
    }
}

/* {a="x",b="y"} with an optional extra label (le) */
static void sb_labels(sbuf *b, const av_mfamily *m, const series *s, const char *extra_name,
                      const char *extra_val)
{
    if (m->n_labels == 0 && extra_name == NULL) {
        return;
    }
    sb_str(b, "{");
    for (size_t i = 0; i < m->n_labels; i++) {
        if (i > 0) {
            sb_str(b, ",");
        }
        sb_str(b, m->label_names[i]);
        sb_str(b, "=\"");
        sb_escaped(b, s->labels[i], 1);
        sb_str(b, "\"");
    }
    if (extra_name != NULL) {
        if (m->n_labels > 0) {
            sb_str(b, ",");
        }
        sb_str(b, extra_name);
        sb_str(b, "=\"");
        sb_str(b, extra_val);
        sb_str(b, "\"");
    }
    sb_str(b, "}");
}

static void render_family(sbuf *b, av_mfamily *m)
{
    static const char *const type_names[] = {"counter", "gauge", "histogram"};
    pthread_mutex_lock(&m->mu);
    sb_str(b, "# HELP ");
    sb_str(b, m->name);
    sb_str(b, " ");
    sb_escaped(b, m->help, 0);
    sb_str(b, "\n# TYPE ");
    sb_str(b, m->name);
    sb_str(b, " ");
    sb_str(b, type_names[m->type]);
    sb_str(b, "\n");
    for (size_t i = 0; i < m->n_series; i++) {
        const series *s = &m->s[i];
        if (m->type != AV_MTYPE_HISTOGRAM) {
            sb_str(b, m->name);
            sb_labels(b, m, s, NULL, NULL);
            sb_str(b, " ");
            sb_num(b, s->value);
            sb_str(b, "\n");
            continue;
        }
        uint64_t cum = 0;
        for (size_t k = 0; k <= m->n_bounds; k++) {
            cum += s->bucket[k];
            char le[40];
            if (k < m->n_bounds) {
                snprintf(le, sizeof(le), "%.9g", m->bounds[k]);
            } else {
                snprintf(le, sizeof(le), "+Inf");
            }
            sb_str(b, m->name);
            sb_str(b, "_bucket");
            sb_labels(b, m, s, "le", le);
            sb_str(b, " ");
            sb_num(b, (double) cum);
            sb_str(b, "\n");
        }
        sb_str(b, m->name);
        sb_str(b, "_sum");
        sb_labels(b, m, s, NULL, NULL);
        sb_str(b, " ");
        sb_num(b, s->value);
        sb_str(b, "\n");
        sb_str(b, m->name);
        sb_str(b, "_count");
        sb_labels(b, m, s, NULL, NULL);
        sb_str(b, " ");
        sb_num(b, (double) s->count);
        sb_str(b, "\n");
    }
    pthread_mutex_unlock(&m->mu);
}

char *av_metrics_render(size_t *len)
{
    sbuf b = {NULL, 0, 0};
    sb_str(&b, ""); /* make sure the buffer exists even with no families */
    pthread_mutex_lock(&g_reg_mu);
    for (av_mfamily *m = g_head; m != NULL; m = m->next) {
        render_family(&b, m);
    }
    pthread_mutex_unlock(&g_reg_mu);
    if (b.p == NULL) {
        return NULL;
    }
    if (len != NULL) {
        *len = b.len;
    }
    return b.p;
}
