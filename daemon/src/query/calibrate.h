/*
 * query/calibrate.h: Startup calibration of distance calls and HNSW searches (c_d and rho).
 */
#ifndef AV_QUERY_CALIBRATE_H
#define AV_QUERY_CALIBRATE_H

#include <stdbool.h>
#include <stddef.h>

#include "kernels/opclass.h"
#include "log.h"

typedef struct {
    double c_d_ns;     /* time in ns for one distance call */
    double rho;        /* random-access penalty rho (~2.0 - 4.0) */
    double gamma;      /* sqrt(rho), default 1.7 */
} av_calibration;

/* Run startup calibration */
av_status av_calibrate_startup(av_calibration *out, av_err *e);

/* Access or update calibration */
const av_calibration *av_calibration_get(void);
void av_calibration_set(const av_calibration *c);

/* Online refinement */
void av_calibration_record_flat(double elapsed_ns, size_t count);
void av_calibration_record_hnsw(double elapsed_ns, size_t count);

#endif /* AV_QUERY_CALIBRATE_H */
