#include <stdint.h>
#include <limits.h>  // 补充INT_MAX/INT_MIN定义
#include "mp3common.h"
#include "assembly.h"

// 直接使用Helix-MP3固定的移位位数（16位）
#define MUL_SHIFT_BITS 16  

// 实现：2个int参数，乘法后右移16位（匹配库要求）
int xmp3_MULSHIFT32(int x, int y) {
    // 注意：Helix-MP3中实际是(int32_t)x * (int32_t)y，结果右移16位（无需int64_t，避免额外截断）
    int32_t product = (int32_t)x * y;
    // 算术右移16位（保留符号位）
    int result = (int)(product >> 16);
    // 原生Helix-MP3不做饱和截断（饱和会导致算法精度丢失）
    return result;
}

// 快速绝对值（int类型）
int xmp3_FASTABS(int x) {
    if (x == INT_MIN) { // 处理INT_MIN的特殊情况
        return INT_MAX;
    }
    int mask = x >> (sizeof(int)*8 - 1);
    return (x ^ mask) - mask;
}