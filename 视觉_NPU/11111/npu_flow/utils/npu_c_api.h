/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#ifndef __NPU_RUNTIME_C_API_H__
#define __NPU_RUNTIME_C_API_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include "al_type.h"

#define YUV_444 0
#define YUV_422 1
#define YUV_420 2

// typedef struct npu_input_quant_param {
    
//     /* for input quant */
//     AL_S8 r_mean_rshift;
//     AL_S16 r_mean_param;
//     AL_S8 g_mean_rshift;
//     AL_S16 g_mean_param;
//     AL_S8 b_mean_rshift;
//     AL_S16 b_mean_param;
//     AL_S8 r_quant_rshift;
//     AL_S16 r_quant_param;
//     AL_S8 g_quant_rshift;
//     AL_S16 g_quant_param;
//     AL_S8 b_quant_rshift;
//     AL_S16 b_quant_param;
//     AL_S32 input_zero_point;

// } npu_input_quant_param_t;

typedef struct predictions {
    int label;
    int x_l;
    int y_l;
    int x_h;
    int y_h;
    float prob;
    const char *label_name;
} predictions_t;

typedef struct {
    float scale;
    AL_S32 zero_point;
} quant_info_t;

/** @} */ /* End of group npu runtime */

/** @} */ /* End of group Anlogic */
#ifdef __cplusplus
}
#endif

#endif
