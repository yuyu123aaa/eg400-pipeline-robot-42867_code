#ifndef AL_NPU_PREPROCESS
#define AL_NPU_PREPROCESS

#include "executor.hpp"
#include "armnnALUtils/DataProcess.hpp"
#include <array>
#include <opencv2/opencv.hpp>

struct ScaleQaunt {
    int org_h = 0;
    int org_w = 0;
    int new_h = 0;
    int new_w = 0;
    uint16_t w_scale_param = 0;
    uint8_t w_scale_rshift = 0;
    uint16_t h_scale_param = 0;
    uint8_t h_scale_rshift = 0;
};

void prepare_input_data(cv::Mat &raw_pic, const Size &input_size, \
    		npu_input_quant_param_t &npu_input_quant_param
			, armnn::ConstTensor& input_tensor);

void fixed_parameter (float f_param, uint16_t* i_param, uint8_t* i_rshift);

bool prepare_input_data(uint8_t* src_addr, ScaleQaunt &sq, npu_input_quant_param_t quant_param, uint8_t* dst_addr);

bool prepare_input_data(uint8_t *src_addr, int input_h, int input_w, npu_input_quant_param_t quant_param, uint8_t *dst_addr) __attribute__((optimize("O3")));

void npu_input_quant_param_init(npu_input_quant_param_t &npu_input_quant_param,
                                float input_scale, int input_zero, armnn::DataType network_data_type,
                                std::array<float, 3> scale = {(1. / 255.), (1. / 255.), (1. / 255.)},
                                std::array<float, 3> mean = {0, 0, 0});
template <typename T, size_t N, typename T2, size_t N2>
static inline void npu_input_quant_param_init(npu_input_quant_param_t &npu_input_quant_param,
                                              float input_scale, int input_zero, armnn::DataType network_data_type,
                                              const T (&scale)[N], const T2 (&mean)[N2]) {
    static_assert(N == 3, "Only Array which Size==3 be Accept");
    static_assert(N2 == 3, "Only Array which Size==3 be Accept");
    npu_input_quant_param_init(
        npu_input_quant_param, input_scale, input_zero, network_data_type,
        std::array<float, 3>{scale[0], scale[1], scale[2]},
        std::array<float, 3>{mean[0], mean[1], mean[2]});
}

static inline void npu_input_quant_param_init(npu_input_quant_param_t &npu_input_quant_param,
                                              float input_scale, int input_zero, armnn::DataType network_data_type,
                                              float scale, float mean) {
    npu_input_quant_param_init(
        npu_input_quant_param, input_scale, input_zero, network_data_type,
        std::array<float, 3>{scale, scale, scale},
        std::array<float, 3>{mean, mean, mean});
}

bool prepare_input_data_float(uint8_t* src_addr, int input_h, int input_w, const float mean[3], const float scale[3], float* dst_addr);

void soft_resize_yuv422_to_rgb_plane(uint8_t *src, uint8_t *dst, int src_h, int src_w, int dst_h, int dst_w, bool letterbox = false) __attribute__((optimize("O3")));
// todo: add more resize functions
void soft_resize_bgra_to_rgb_plane(uint8_t *src, uint8_t *dst, int src_h, int src_w, int dst_h, int dst_w) __attribute__((optimize("O3")));

#endif