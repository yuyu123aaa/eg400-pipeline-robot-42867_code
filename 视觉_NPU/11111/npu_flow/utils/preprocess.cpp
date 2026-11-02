#include "preprocess.hpp"

#define MAX_RSHIFT        16
#define MAX_FX_PARAM_BITS 16
#define RSHIFT_THRESHOLD  ((1 << (MAX_RSHIFT - 2)) - 1)
// util function
void fixed_parameter (float f_param, uint16_t* i_param, uint8_t* i_rshift)
{
    uint8_t i_rs = 0 ;

    for (i_rs = 0; i_rs < MAX_RSHIFT; i_rs++) {
        if (fabsf(f_param * (1 << i_rs)) > RSHIFT_THRESHOLD)
            break ;
    }

    *i_param  = f_param * (1 << i_rs) ;
    *i_rshift = i_rs ;

    return ;
}

bool prepare_input_data(uint8_t* src_addr, ScaleQaunt &sq, npu_input_quant_param_t quant_param, uint8_t* dst_addr) {
    int r, c, k;

    uint16_t w_half_value = 1 << (sq.w_scale_rshift - 1);
    uint16_t h_half_value = 1 << (sq.h_scale_rshift - 1);
    
    for(r = 0; r < sq.new_h; ++r) {
        // obtain loc low and high, as well as fraction
        int32_t sy = r * sq.h_scale_param + (sq.h_scale_param >> 1) - h_half_value;

        if (sy < 0) 
            sy = 0;
        int iy = (sy + h_half_value)>> sq.h_scale_rshift;
        if (iy >= sq.org_h - 1) {
            iy = sq.org_h -1;
        }

        for(c = 0; c < sq.new_w; ++c) {
            int32_t sx = c * sq.w_scale_param + (sq.w_scale_param >> 1) - w_half_value;

            if (sx < 0) 
                sx = 0;
            int ix = (sx + w_half_value) >> sq.w_scale_rshift;
            if (ix >= sq.org_w - 1) {
                ix = sq.org_w -1;
            }

            for(k = 0; k < 3; ++k) {
                int32_t val = 0;
                val = src_addr[iy*sq.org_w*4 + ix*4 + k];

                int val_int = val;
                int idata = 0;

                switch(k) {
                    case 2:
                    {
                        // idata =(( (((val_int<<p_mean_rshift[c]) - p_mean_param[c]) >>p_mean_rshift[c]) * p_u_param[c] + (1 <<(p_u_rshift[c]-1)) ) >> p_u_rshift[c] )+ (zero_point);
                        idata =(( (((val_int<<quant_param.r_mean_rshift) - quant_param.r_mean_param) >>quant_param.r_mean_rshift) * quant_param.r_quant_param + \
                                    (1 <<(quant_param.r_quant_rshift-1)) ) >> quant_param.r_quant_rshift )+ (quant_param.input_zero_point);
                        break;
                    }
                    case 1:
                    {
                        idata =(( (((val_int<<quant_param.g_mean_rshift) - quant_param.g_mean_param) >>quant_param.g_mean_rshift) * quant_param.g_quant_param + \
                                    (1 <<(quant_param.g_quant_rshift-1)) ) >> quant_param.g_quant_rshift )+ (quant_param.input_zero_point);
                        break;
                    }
                    case 0:
                    {
                        idata =(( (((val_int<<quant_param.b_mean_rshift) - quant_param.b_mean_param) >>quant_param.b_mean_rshift) * quant_param.b_quant_param + \
                                    (1 <<(quant_param.b_quant_rshift-1)) ) >> quant_param.b_quant_rshift )+ (quant_param.input_zero_point);
                        break;
                    }
                    default:
                    {   
                        printf("Error: things go wrong\n");
                        assert(0);
                        break;
                    }
                }
                // int idata =(( ((((int)img_data[in_index]<<p_mean_rshift[c]) - p_mean_param[c]) >>p_mean_rshift[c]) * p_u_param[c] + (1 <<(p_u_rshift[c]-1)) ) >> p_u_rshift[c] )+ (zero_point);
                if (quant_param.quant_type == 1) {
                    if (idata > 127)
                        idata = 127;
                    else if (idata < -128)
                        idata = -128;
                } else {
                    if (idata > 255)
                        idata = 255;
                    else if (idata < 0)
                        idata = 0;
                }
				switch (k) {
					case 0:
					{
						dst_addr[k*sq.new_h*sq.new_w + r*sq.new_w + 2] = idata;
						break;
					}
					case 1:
					{
						dst_addr[k*sq.new_h*sq.new_w + r*sq.new_w + 1] = idata;
						break;
					}
					case 2:
					{
						dst_addr[k*sq.new_h*sq.new_w + r*sq.new_w + 0] = idata;
						break;
					}
					default:
					{
						printf("Error: things go wrong\n");
                        assert(0);
                        break;
					}
				}
            }
        }
    }
    return true;
}

bool prepare_input_data(uint8_t* src_addr, int input_h, int input_w, npu_input_quant_param_t quant_param, uint8_t* dst_addr) {
    int r, c, k;
    
    for(r = 0; r < input_h; ++r) {
        for(c = 0; c < input_w; ++c) {
            for(k = 0; k < 3; ++k) {
                int32_t val_int = 0;
                val_int = src_addr[r*input_w*3 + c*3 + k];
                int idata = 0;

                switch(k) {
                    case 2:
                    {
                        // idata =(( (((val_int<<p_mean_rshift[c]) - p_mean_param[c]) >>p_mean_rshift[c]) * p_u_param[c] + (1 <<(p_u_rshift[c]-1)) ) >> p_u_rshift[c] )+ (zero_point);
                        idata =(( (((val_int<<quant_param.r_mean_rshift) - quant_param.r_mean_param) >>quant_param.r_mean_rshift) * quant_param.r_quant_param + \
                                    (1 <<(quant_param.r_quant_rshift-1)) ) >> quant_param.r_quant_rshift )+ (quant_param.input_zero_point);
                        break;
                    }
                    case 1:
                    {
                        idata =(( (((val_int<<quant_param.g_mean_rshift) - quant_param.g_mean_param) >>quant_param.g_mean_rshift) * quant_param.g_quant_param + \
                                    (1 <<(quant_param.g_quant_rshift-1)) ) >> quant_param.g_quant_rshift )+ (quant_param.input_zero_point);
                        break;
                    }
                    case 0:
                    {
                        idata =(( (((val_int<<quant_param.b_mean_rshift) - quant_param.b_mean_param) >>quant_param.b_mean_rshift) * quant_param.b_quant_param + \
                                    (1 <<(quant_param.b_quant_rshift-1)) ) >> quant_param.b_quant_rshift )+ (quant_param.input_zero_point);
                        break;
                    }
                    default:
                    {   
                        printf("Error: things go wrong\n");
                        assert(0);
                        break;
                    }
                }
                // int idata =(( ((((int)img_data[in_index]<<p_mean_rshift[c]) - p_mean_param[c]) >>p_mean_rshift[c]) * p_u_param[c] + (1 <<(p_u_rshift[c]-1)) ) >> p_u_rshift[c] )+ (zero_point);
                if (quant_param.quant_type == 1) {
                    if (idata > 127)
                        idata = 127;
                    else if (idata < -128)
                        idata = -128;
                } else {
                    if (idata > 255)
                        idata = 255;
                    else if (idata < 0)
                        idata = 0;
                }
				switch (k) {
					case 0:
					{
						dst_addr[2*input_h*input_w + r*input_w + c] = idata;
						break;
					}
					case 1:
					{
						dst_addr[1*input_h*input_w + r*input_w + c] = idata;
						break;
					}
					case 2:
					{
						dst_addr[0*input_h*input_w + r*input_w + c] = idata;
						break;
					}
					default:
					{
						printf("Error: things go wrong\n");
                        assert(0);
                        break;
					}
				}
            }
        }
    }
    return true;
}
void npu_input_quant_param_init(npu_input_quant_param_t &npu_input_quant_param,
                                float input_scale, int input_zero, armnn::DataType network_data_type,
                                std::array<float, 3> scale,
                                std::array<float, 3> mean) {
    uint16_t p_u_param[3];
    uint8_t p_u_rshift[3];
    uint16_t p_mean_param[3];
    uint8_t p_mean_rshift[3];
    for (int c = 0; c < 3; c++) {
        fixed_parameter(scale[c] / input_scale, &p_u_param[c], &p_u_rshift[c]);
        fixed_parameter(mean[c], &p_mean_param[c], &p_mean_rshift[c]);
    }

    npu_input_quant_param.r_mean_rshift = p_mean_rshift[0];
    npu_input_quant_param.r_mean_param = p_mean_param[0];
    npu_input_quant_param.g_mean_rshift = p_mean_rshift[1];
    npu_input_quant_param.g_mean_param = p_mean_param[1];
    npu_input_quant_param.b_mean_rshift = p_mean_rshift[2];
    npu_input_quant_param.b_mean_param = p_mean_param[2];

    npu_input_quant_param.r_quant_rshift = p_u_rshift[0];
    npu_input_quant_param.r_quant_param = p_u_param[0];
    npu_input_quant_param.g_quant_rshift = p_u_rshift[1];
    npu_input_quant_param.g_quant_param = p_u_param[1];
    npu_input_quant_param.b_quant_rshift = p_u_rshift[2];
    npu_input_quant_param.b_quant_param = p_u_param[2];

    npu_input_quant_param.input_zero_point = input_zero;

    if (network_data_type == armnn::DataType::QSymmS8
        || (network_data_type == armnn::DataType::QAsymmS8 && input_zero == 0)) {
        npu_input_quant_param.quant_type = 1;
    } else if (network_data_type == armnn::DataType::QAsymmU8) {
        npu_input_quant_param.quant_type = 0;
    } else {
        std::cerr << ("unsupported network quantization type") << std::endl;
        throw std::runtime_error("unsupported network quantization type");
    }
}

bool prepare_input_data_float(uint8_t* src_addr, int input_h, int input_w, const float mean[3], const float scale[3], float* dst_addr) {
    int r, c, k;
    
    for(r = 0; r < input_h; ++r) {
        for(c = 0; c < input_w; ++c) {
            for(k = 0; k < 3; ++k) {
                int32_t val_int = 0;
                val_int = src_addr[r*input_w*3 + c*3 + k];
                float idata = 0;

                switch(k) {
                    case 2:
                    {
                        
                        idata = ((float)val_int - mean[0]) * scale[0];
                        break;
                    }
                    case 1:
                    {
                        idata = ((float)val_int - mean[1]) * scale[1];
                        break;
                    }
                    case 0:
                    {
                        idata = ((float)val_int - mean[2]) * scale[2];
                        break;
                    }
                    default:
                    {   
                        printf("Error: things go wrong\n");
                        assert(0);
                        break;
                    }
                }

				switch (k) {
					case 0:
					{
						dst_addr[2*input_h*input_w + r*input_w + c] = idata;
						break;
					}
					case 1:
					{
						dst_addr[1*input_h*input_w + r*input_w + c] = idata;
						break;
					}
					case 2:
					{
						dst_addr[0*input_h*input_w + r*input_w + c] = idata;
						break;
					}
					default:
					{
						printf("Error: things go wrong\n");
                        assert(0);
                        break;
					}
				}
            }
        }
    }
    return true;
}
union q16 {
    uint16_t u[2];
    uint32_t ul;
};
#include <arm_neon.h>
void soft_resize_yuv422_to_rgb_plane(uint8_t *src, uint8_t *dst, int src_h, int src_w, int dst_h, int dst_w, bool letterbox) {
#define BATCH 8
#define RHS (2)
#define LHS (8 - RHS) // 降一点精度确保不会溢出

    uint8_t y[BATCH], u[BATCH], v[BATCH];

    int pad_left = 0;
    int pad_top = 0;

    // 使用算法，最临近采样
    q16 r_step, c_step;
    if (!letterbox) {
        r_step.ul = (src_h << 16) / dst_h;
        c_step.ul = (src_w << 15) / dst_w; // 这里是15 后面还得乘2，确保访问顺序是yuyv
    } else {
        if (src_w * dst_h >= src_h * dst_w) {
            pad_top = (dst_h - dst_w * src_h / src_w) / 2;
            r_step.ul = (src_w << 16) / dst_w;
            c_step.ul = (src_w << 15) / dst_w;
        } else {
            pad_left = (dst_w - dst_h * src_w / src_h) / 2;
            r_step.ul = (src_h << 16) / dst_h;
            c_step.ul = (src_h << 15) / dst_h;
        }
    }

    uint8_t *pR;
    uint8_t *pG;
    uint8_t *pB;

    q16 sr, sc;

    uint32_t base1;
    uint32_t offset;
    sr.ul = 0;
    for (uint16_t dr = pad_top; dr < dst_h - pad_top; dr++, sr.ul += r_step.ul) {
        sc.ul = 0;
        base1 = 2 * sr.u[1] * src_w;

        pR = dst + dr * dst_w + pad_left;
        pG = dst + dr * dst_w + pad_left + dst_h * dst_w;
        pB = dst + dr * dst_w + pad_left + dst_h * dst_w * 2;

        for (uint16_t dc = pad_left; dc < (dst_w - pad_left) / BATCH; dc++) {
            for (size_t i = 0; i < BATCH; i++, sc.ul += c_step.ul) {
                offset = 2 * 2 * sc.u[1] + base1;
                y[i] = (src[offset] + src[offset + 2]) / 2;
                u[i] = src[offset + 1];
                v[i] = src[offset + 3];
            }
            auto Y = vmull_u8(vld1_u8(y), vdup_n_u8(1 << LHS));
            auto U = vld1_u8(u);
            auto V = vld1_u8(v);

            auto R = vmull_u8(V, vdup_n_u8(360 >> RHS));
            R = vaddq_u16(R, Y);                                // 先常规加法
            R = vqsubq_u16(R, vdupq_n_u16((128 >> RHS) * 360)); // 后饱和减法
            R = vshrq_n_u16(R, LHS);

            auto G1 = vmull_u8(U, vdup_n_u8(88 >> RHS));
            auto G2 = vmull_u8(V, vdup_n_u8(184 >> RHS));
            auto Gt = vaddq_u16(G1, G2);
            auto G = vaddq_u16(Y, vdupq_n_u16((128 >> RHS) * (88 + 184)));
            G = vqsubq_u16(G, Gt);
            G = vshrq_n_u16(G, LHS);

            auto B = vmull_u8(U, vdup_n_u8(455 >> RHS));
            B = vaddq_u16(B, Y);
            B = vqsubq_u16(B, vdupq_n_u16((128 >> RHS) * 455));
            B = vshrq_n_u16(B, LHS);

            vst1_u8(pR, vqmovn_u16(R));
            vst1_u8(pG, vqmovn_u16(G));
            vst1_u8(pB, vqmovn_u16(B));
            pR += BATCH;
            pG += BATCH;
            pB += BATCH;
        }
    }
#undef BATCH
#undef RHS
#undef LHS
}