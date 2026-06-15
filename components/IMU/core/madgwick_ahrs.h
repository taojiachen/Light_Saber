#ifndef MADGWICK_AHRS_H
#define MADGWICK_AHRS_H

#include "imu_port.h"

typedef struct {
    float q0, q1, q2, q3;
    float beta;
    float sample_freq;
} madgwick_ahrs_t;

void madgwick_update(madgwick_ahrs_t *ahrs, const imu_data_t *data);
void madgwick_get_euler(const madgwick_ahrs_t *ahrs, float *roll, float *pitch, float *yaw);

#endif