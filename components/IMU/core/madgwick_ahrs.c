#include "madgwick_ahrs.h"
#include "imu_port.h"
#include <math.h>
#include <stddef.h>   // for NULL
#include "esp_log.h"
static const char *TAG_DRV = "madgwick_ahrs";

#define M_PI_F                  3.14159265358979323846f

/* -------------------------------------------------------------------------- */
/* 内部辅助函数                                                               */
/* -------------------------------------------------------------------------- */

/** 快速逆平方根 (可换用 1.0f / sqrtf(x)) */
static inline float inv_sqrt(float x) {
    return 1.0f / sqrtf(x);
}

/* -------------------------------------------------------------------------- */
/* 九轴 Madgwick 融合（加速度 + 陀螺仪 + 磁力计）                              */
/* 参考: Madgwick's AHRS algorithm (2010)                                     */
/* -------------------------------------------------------------------------- */
static void madgwick_ahrs_update_9dof(
    madgwick_ahrs_t *ahrs,
    float ax, float ay, float az,   /* 加速度 (g) */
    float gx, float gy, float gz,   /* 陀螺仪 (dps) */
    float mx, float my, float mz,   /* 磁力计 (uT 或任意单位，仅方向有用) */
    int32_t *active_level)          /* 活跃等级 */
{
    float recip_norm;
    float s0, s1, s2, s3;
    float q_dot0, q_dot1, q_dot2, q_dot3;
    float hx, hy;
    float _2q0mx, _2q0my, _2q0mz, _2q1mx, _2bx, _2bz, _4bx, _4bz;
    float _2q0, _2q1, _2q2, _2q3, _2q0q2, _2q2q3;
    float q0q0, q0q1, q0q2, q0q3, q1q1, q1q2, q1q3, q2q2, q2q3, q3q3;

    /* 取得当前四元数 */
    float q0 = ahrs->q0;
    float q1 = ahrs->q1;
    float q2 = ahrs->q2;
    float q3 = ahrs->q3;
    float beta = ahrs->beta;

    /* 加速度归一化 */
    recip_norm = inv_sqrt(ax * ax + ay * ay + az * az);
    ax *= recip_norm;
    ay *= recip_norm;
    az *= recip_norm;

    /* 磁力计归一化 */
    recip_norm = inv_sqrt(mx * mx + my * my + mz * mz);
    mx *= recip_norm;
    my *= recip_norm;
    mz *= recip_norm;

    /* 辅助变量 */
    _2q0mx = 2.0f * q0 * mx;
    _2q0my = 2.0f * q0 * my;
    _2q0mz = 2.0f * q0 * mz;
    _2q1mx = 2.0f * q1 * mx;
    _2q0 = 2.0f * q0;
    _2q1 = 2.0f * q1;
    _2q2 = 2.0f * q2;
    _2q3 = 2.0f * q3;
    _2q0q2 = 2.0f * q0 * q2;
    _2q2q3 = 2.0f * q2 * q3;
    q0q0 = q0 * q0;
    q0q1 = q0 * q1;
    q0q2 = q0 * q2;
    q0q3 = q0 * q3;
    q1q1 = q1 * q1;
    q1q2 = q1 * q2;
    q1q3 = q1 * q3;
    q2q2 = q2 * q2;
    q2q3 = q2 * q3;
    q3q3 = q3 * q3;

    /* 参考磁场方向 (hx, hy, _2bz) */
    hx = mx * q0q0 - _2q0my * q3 + _2q0mz * q2 + mx * q1q1
         + _2q1 * my * q2 + _2q1 * mz * q3 - mx * q2q2 - mx * q3q3;
    hy = _2q0mx * q3 + my * q0q0 - _2q0mz * q1 + _2q1mx * q2
         - my * q1q1 + my * q2q2 + _2q2 * mz * q3 - my * q3q3;
    _2bx = sqrtf(hx * hx + hy * hy);
    _2bz = -_2q0mx * q2 + _2q0my * q1 + mz * q0q0 + _2q1mx * q3
           - mz * q1q1 + _2q2 * my * q3 - mz * q2q2 + mz * q3q3;
    _4bx = 2.0f * _2bx;
    _4bz = 2.0f * _2bz;

    /* 梯度下降修正步长（计算目标函数的雅可比） */
    s0 = -_2q2 * (2.0f * q1q3 - _2q0q2 - ax)
         + _2q1 * (2.0f * q0q1 + _2q2q3 - ay)
         - _2bz * q2 * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
         + (-_2bx * q3 + _2bz * q1) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
         + _2bx * q2 * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);

    s1 = _2q3 * (2.0f * q1q3 - _2q0q2 - ax)
         + _2q0 * (2.0f * q0q1 + _2q2q3 - ay)
         - 4.0f * q1 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - ax)
         + _2bz * q3 * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
         + (_2bx * q2 + _2bz * q0) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
         + (_2bx * q3 - _4bz * q1) * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);

    s2 = -_2q0 * (2.0f * q1q3 - _2q0q2 - ax)
         + _2q3 * (2.0f * q0q1 + _2q2q3 - ay)
         - 4.0f * q2 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - ax)
         + (-_4bx * q2 - _2bz * q0) * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
         + (_2bx * q1 + _2bz * q3) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
         + (_2bx * q0 - _4bz * q2) * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);

    s3 = _2q1 * (2.0f * q1q3 - _2q0q2 - ax)
         + _2q2 * (2.0f * q0q1 + _2q2q3 - ay)
         + (-_4bx * q3 + _2bz * q1) * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
         + (-_2bx * q0 + _2bz * q2) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
         + _2bx * q1 * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);

    /* 归一化步长 */
    recip_norm = inv_sqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
    s0 *= recip_norm;
    s1 *= recip_norm;
    s2 *= recip_norm;
    s3 *= recip_norm;

    /* 陀螺仪数据转为弧度/秒 */
    float gx_rad = gx * 0.0174533f;   /* PI / 180 */
    float gy_rad = gy * 0.0174533f;
    float gz_rad = gz * 0.0174533f;

    /* 四元数变化率（运动学方程） - 梯度修正 */
    q_dot0 = 0.5f * (-q1 * gx_rad - q2 * gy_rad - q3 * gz_rad) - beta * s0;
    q_dot1 = 0.5f * ( q0 * gx_rad + q2 * gz_rad - q3 * gy_rad) - beta * s1;
    q_dot2 = 0.5f * ( q0 * gy_rad - q1 * gz_rad + q3 * gx_rad) - beta * s2;
    q_dot3 = 0.5f * ( q0 * gz_rad + q1 * gy_rad - q2 * gx_rad) - beta * s3;

    /* 一阶积分更新四元数 */
    q0 += q_dot0 * (1.0f / ahrs->sample_freq);
    q1 += q_dot1 * (1.0f / ahrs->sample_freq);
    q2 += q_dot2 * (1.0f / ahrs->sample_freq);
    q3 += q_dot3 * (1.0f / ahrs->sample_freq);

    /* 四元数归一化 */
    recip_norm = inv_sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    ahrs->q0 = q0 * recip_norm;
    ahrs->q1 = q1 * recip_norm;
    ahrs->q2 = q2 * recip_norm;
    ahrs->q3 = q3 * recip_norm;
}

/* -------------------------------------------------------------------------- */
/* 六轴 Madgwick 融合（加速度 + 陀螺仪，无磁力计）                             */
/* 参考: Madgwick's IMU algorithm                                             */
/* -------------------------------------------------------------------------- */
static void madgwick_ahrs_update_6dof(
    madgwick_ahrs_t *ahrs,
    float ax, float ay, float az,
    float gx, float gy, float gz,
    int32_t *active_level)
{
    float recipNorm;
    float s0, s1, s2, s3;
    float qDot1, qDot2, qDot3, qDot4;
    float _4q0, _4q1, _4q2, _8q1, _8q2, _2q0, _2q1, _2q2, _2q3, q0q0, q1q1, q2q2, q3q3;

    // 使用IMU算法（不使用磁力计）
    // 将角速度从度/秒转换为弧度/秒
    gx *= (M_PI_F / 180.0f);
    gy *= (M_PI_F / 180.0f);
    gz *= (M_PI_F / 180.0f);

    // 四元数导数的计算
    qDot1 = 0.5f * (-ahrs->q1 * gx - ahrs->q2 * gy - ahrs->q3 * gz);
    qDot2 = 0.5f * (ahrs->q0 * gx + ahrs->q2 * gz - ahrs->q3 * gy);
    qDot3 = 0.5f * (ahrs->q0 * gy - ahrs->q1 * gz + ahrs->q3 * gx);
    qDot4 = 0.5f * (ahrs->q0 * gz + ahrs->q1 * gy - ahrs->q2 * gx);

    // 计算设备活动等级（高频加速度）
    if (active_level != NULL) {
        // 使用重力向量作为参考点
        float gx_ref = 2.0f * (ahrs->q1 * ahrs->q3 - ahrs->q0 * ahrs->q2);
        float gy_ref = 2.0f * (ahrs->q0 * ahrs->q1 + ahrs->q2 * ahrs->q3);
        float gz_ref = ahrs->q0 * ahrs->q0 - ahrs->q1 * ahrs->q1 - ahrs->q2 * ahrs->q2 + ahrs->q3 * ahrs->q3;

        // 计算当前加速度向量的模长
        float acc_magnitude = sqrtf(ax * ax + ay * ay + az * az);

        // 计算重力向量的模长（应该接近1）
        float gravity_magnitude = sqrtf(gx_ref * gx_ref + gy_ref * gy_ref + gz_ref * gz_ref);

        // 计算加速度与重力之间的夹角余弦值
        float dot_product = ax * gx_ref + ay * gy_ref + az * gz_ref;
        float cos_angle   = dot_product / (acc_magnitude * gravity_magnitude + 1e-10f); // 防止除零

        // 限制cos_angle在[-1, 1]范围内，防止sqrt计算出错
        if (cos_angle > 1.0f) {
            cos_angle = 1.0f;
        }
        if (cos_angle < -1.0f) {
            cos_angle = -1.0f;
        }

        // 计算垂直于重力方向的加速度分量（运动分量）
        float motion_component = acc_magnitude * sqrtf(1.0f - cos_angle * cos_angle);

        // 转换为mg单位并取整
        *active_level = (int)(motion_component * 1000.0f);
    }
    // 如果加速度计数据有效，则计算反馈
    if (!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f))) {

        float acc_magnitude_sq = ax * ax + ay * ay + az * az;
        if (acc_magnitude_sq > 1e-7f) { // 添加防除零检查   使用1e-7f作为阈值避免浮点精度问题

            // 归一化加速度计测量值
            recipNorm = 1.0f / sqrtf(ax * ax + ay * ay + az * az);
            ax *= recipNorm;
            ay *= recipNorm;
            az *= recipNorm;
        } else {
            // 处理无效加速度数据
            ESP_LOGE(TAG_DRV, "无效加速度计数据");
            recipNorm = 0.0f;
        }

        // 从四元数计算重力方向的参考向量
        _2q0 = 2.0f * ahrs->q0;
        _2q1 = 2.0f * ahrs->q1;
        _2q2 = 2.0f * ahrs->q2;
        _2q3 = 2.0f * ahrs->q3;
        _4q0 = 4.0f * ahrs->q0;
        _4q1 = 4.0f * ahrs->q1;
        _4q2 = 4.0f * ahrs->q2;
        _8q1 = 8.0f * ahrs->q1;
        _8q2 = 8.0f * ahrs->q2;
        q0q0 = ahrs->q0 * ahrs->q0;
        q1q1 = ahrs->q1 * ahrs->q1;
        q2q2 = ahrs->q2 * ahrs->q2;
        q3q3 = ahrs->q3 * ahrs->q3;

        // 梯度下降算法的目标函数
        s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
        s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * ahrs->q1 - _2q0 * ay - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
        s2 = 4.0f * q0q0 * ahrs->q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
        s3 = 4.0f * q1q1 * ahrs->q3 - _2q1 * ax + 4.0f * q2q2 * ahrs->q3 - _2q2 * ay;

        float s_magnitude_sq = s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;
        if (s_magnitude_sq > 1e-7f) { // 添加防除零检查  使用1e-7f作为阈值避免浮点精度问题
            recipNorm = 1.0f / sqrtf(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
            s0 *= recipNorm;
            s1 *= recipNorm;
            s2 *= recipNorm;
            s3 *= recipNorm;
        } else {
            // 处理无效目标函数
            recipNorm = 0.0f;
            s0 = s1 = s2 = s3 = 0.0f;
        }

        // 应用反馈步骤
        qDot1 -= ahrs->beta * s0;
        qDot2 -= ahrs->beta * s1;
        qDot3 -= ahrs->beta * s2;
        qDot4 -= ahrs->beta * s3;
    } else {
        ESP_LOGE(TAG_DRV, "无效加速度计数据");
    }

    // 积分四元数导数
    ahrs->q0 += qDot1 * (1.0f / ahrs->sample_freq);
    ahrs->q1 += qDot2 * (1.0f / ahrs->sample_freq);
    ahrs->q2 += qDot3 * (1.0f / ahrs->sample_freq);
    ahrs->q3 += qDot4 * (1.0f / ahrs->sample_freq);

    // 归一化四元数
    recipNorm = 1.0f / sqrtf(ahrs->q0 * ahrs->q0 + ahrs->q1 * ahrs->q1 + ahrs->q2 * ahrs->q2 + ahrs->q3 * ahrs->q3);
    ahrs->q0 *= recipNorm;
    ahrs->q1 *= recipNorm;
    ahrs->q2 *= recipNorm;
    ahrs->q3 *= recipNorm;
}

/* -------------------------------------------------------------------------- */
/* 公共 API                                                                   */
/* -------------------------------------------------------------------------- */
void madgwick_update(madgwick_ahrs_t *ahrs, const imu_data_t *data) {
    if (ahrs == NULL || data == NULL) return;

    /* 根据 data_flags 判断磁力计是否有效，自动切换九轴/六轴 */
    if (data->data_flags & IMU_DATA_MAG_VALID) {
        madgwick_ahrs_update_9dof(ahrs,
            data->accel_x, data->accel_y, data->accel_z,
            data->gyro_x,  data->gyro_y,  data->gyro_z,
            data->mag_x,   data->mag_y,   data->mag_z,
            &data->active_level);
    } else {
        madgwick_ahrs_update_6dof(ahrs,
            data->accel_x, data->accel_y, data->accel_z,
            data->gyro_x,  data->gyro_y,  data->gyro_z,
            &data->active_level);
    }
}

void madgwick_get_euler(const madgwick_ahrs_t *ahrs, float *roll, float *pitch, float *yaw) {
    if (ahrs == NULL || roll == NULL || pitch == NULL || yaw == NULL) return;
    
    // Roll (x-axis rotation)
    float sinr_cosp = 2.0f * (ahrs->q0 * ahrs->q1 + ahrs->q2 * ahrs->q3);
    float cosr_cosp = 1.0f - 2.0f * (ahrs->q1 * ahrs->q1 + ahrs->q2 * ahrs->q2);
    *roll           = atan2f(sinr_cosp, cosr_cosp) * (180.0f / M_PI_F);

    // Pitch (y-axis rotation)：使用asin，扩展至±90°
    float sinp = 2.0f * (ahrs->q0 * ahrs->q2 - ahrs->q1 * ahrs->q3);
    // 限制sinp到[-1,1]，防止数值误差导致asin计算异常
    sinp = fmaxf(fminf(sinp, 1.0f), -1.0f);
    *pitch = asinf(sinp) * (180.0f / M_PI_F);

    // Yaw (z-axis rotation)
    // if (yaw != NULL) {
    //     float siny_cosp = 2.0f * (ahrs->q0 * ahrs->q3 + ahrs->q1 * ahrs->q2);
    //     float cosy_cosp = 1.0f - 2.0f * (ahrs->q2 * ahrs->q2 + ahrs->q3 * ahrs->q3);
    //     *yaw            = atan2f(siny_cosp, cosy_cosp) * (180.0f / M_PI_F);
    // }
}