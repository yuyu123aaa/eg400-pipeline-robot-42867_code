/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/
#include "buffered_executor.hpp"
#include "flow_common_types.hpp"
#include "main_helper.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>
#include "ThreadDataInProcessing.hpp"
#include "armnnALUtils/DataProcess.hpp"
#include "icamera.hpp"
#include "ihdmi.hpp"
#include "udp_send.hpp"

// the namespace only for language servers, IDEs .etc
namespace YDHC {

/**
 * @brief demo flow management class
 *
 */
class solution {
public:
    /**
     * @brief record additional info for each task
     *
     */
    struct task_context {
        unsigned int work_idx = 0;
        struct time_info_t {
            const char *id;
            std::chrono::system_clock::time_point start;
            std::chrono::system_clock::time_point end;
            pthread_t th_id = pthread_self();
        };
        std::vector<time_info_t> time_infos;
    };

    /**
     * @brief execute solution flow
     *
     */
    void main_loop();

    /**
     * @brief execute task: obtain data from camera
     *
     * @param p context for task_camera
     */
    void task_camera(std::shared_ptr<task_context> p);

    /**
     * @brief execute task: resize input data to desired size && execute quant if neccessary
     *
     * @param p context for task_resize
     */
    void task_resize(std::shared_ptr<task_context> p);

    /**
     * @brief execute task: feed input data to AI network && obtain inferenced result
     *
     * @param p context for task_npu
     */
    void task_npu(std::shared_ptr<task_context> p);

    /**
     * @brief execute task: get the detected object from the output
     *
     * @param p context for task_yolo
     */
    void task_yolo(std::shared_ptr<task_context> p);

    /**
     * @brief execute task: draw result on input image && show on hdmi
     *
     * @param p context for task_hdmi
     */
    void task_hdmi(std::shared_ptr<task_context> p);

    /**
     * @brief execute task: profile different tasks && print results
     *
     * @param p context for task_report
     */
    void task_report(std::shared_ptr<task_context> p);

/**
 * @brief you can extend your own task and register it here or you can remove the task you don't need
 * @brief 实例化ThreadDataInProcessing时候使用lambda绑定真正执行的任务，设置流水线的执行顺序、bypass、并行等
 */
#define REGISTER_TASK__(enable_threads, task, product_limit, next_task)               \
    ThreadDataInProcessing<std::shared_ptr<task_context>, enable_threads> ppl_##task{ \
        [this](std::shared_ptr<task_context> p) {                                     \
            if (m_stop_flag) return;                                                  \
            task(p);                                                                  \
            if ((void *)&ppl_##task != (void *)&ppl_##next_task)                      \
                ppl_##next_task.mt_emplace(p);                                        \
        },                                                                            \
        product_limit};
    REGISTER_TASK__(false, task_camera, 1, task_resize);
    REGISTER_TASK__(true, task_resize, 1, task_npu);
    REGISTER_TASK__(true, task_npu, 1, task_yolo);
    REGISTER_TASK__(true, task_yolo, 1, task_hdmi);
    REGISTER_TASK__(false, task_hdmi, 1, task_report);
    REGISTER_TASK__(false, task_report, 1, task_report);
#undef REGISTER_TASK__

    /**
     * @brief load model from disk
     *
     * @param model path to onnx model
     * @param addtions unused
     * @return int
     */
    int loadModel(const char *model, const char *addtions);

    /**
     * @brief Construct a new solution object
     *
     * @param cfg
     * @param config
     * @param _cam
     * @param _hdmi
     */
    explicit solution(app_config cfg, user_config config, std::shared_ptr<icamera> _cam, std::shared_ptr<ihdmi> _hdmi, std::shared_ptr<udp_send> _udp_send);

    /**
     * @brief Destroy the solution object
     *
     */
    ~solution();

    /**
     * @brief Set the input width
     *
     * @param w
     */
    void set_input_width(uint32_t w) { m_input_width = w; }

    /**
     * @brief Set the input height
     *
     * @param h
     */
    void set_input_height(uint32_t h) { m_input_height = h; }

    /**
     * @brief Get the input width
     *
     * @return uint32_t
     */
    uint32_t get_input_width() { return m_input_width; }

    /**
     * @brief Get the input height
     *
     * @return uint32_t
     */
    uint32_t get_input_height() { return m_input_height; }

    /**
     * @brief Set the mean
     *
     * @param r_mean
     * @param g_mean
     * @param b_mean
     */
    void set_mean(float r_mean, float g_mean, float b_mean) {
        m_mean[0] = r_mean;
        m_mean[1] = g_mean;
        m_mean[2] = b_mean;
    }

    /**
     * @brief Set the scale
     *
     * @param r_scale
     * @param g_scale
     * @param b_scale
     */
    void set_scale(float r_scale, float g_scale, float b_scale) {
        m_scale[0] = r_scale;
        m_scale[1] = g_scale;
        m_scale[2] = b_scale;
    }

public:
    /**
     * @brief record the buffer id for the first task in the pipeline
     *
     */
    unsigned int work_buffer_idx = 0;

    /**
     * @brief record the buffer id for the last task in the pipeline
     *
     */
    unsigned int m_last_work_buffer_idx = 0;

private:
    /**
     * @brief Set stop flag for all pipeline
     *
     */
    void set_m_stop_flag() { m_stop_flag = true; }

private:
    bool m_stop_flag = false;

    std::unique_ptr<BufferedNPUExecutor> m_executor;

    // workload for pre process
    std::function<void(const armnn::ConstTensor &inputTensor,
                       const armnn::ConstTensor &outputTensor)>
        m_resize_workload;

    // workload for yolo post processing
    std::function<std::vector<DetectObject>(armnn::OutputTensors &outputTensors)> m_yolo_workload;

    // the number of cma buffers used in the pipeline
    static constexpr unsigned int m_buf_nums = MAX_BUF_NUM;

    // using for m_resize_workload
    armnn::ConstTensor m_cmabufWrapperForReisze[m_buf_nums];

    // store detected objects
    std::vector<DetectObject> m_objects[m_buf_nums];

    // buffers used by camera, also by hdmi
    CmaBuf m_cmabuf[m_buf_nums];

    // record network input width. used by yolo process
    uint32_t m_input_width;

    // record network input height. used by yolo process
    uint32_t m_input_height;

    // mean value for input image r,g,b
    float m_mean[3];

    // scale value for input image r,g,b
    float m_scale[3];

    /**
     * @brief quant parameters calculated using m_mean, m_scale as well as
     *          scale and zero point value of input tensor in AI network.
     *          this parameter can be used by resize or csc module in alnpu.
     */
    npu_input_quant_param_t m_npu_quant_param;

    // config for app behavior
    app_config m_cfg;

    // config for pre&post process
    user_config m_config;

    // handle for camera device
    std::shared_ptr<icamera> m_camera;

    // handle for hdmi device
    std::shared_ptr<ihdmi> m_hdmi;
    
    // handle for udp device
    std::shared_ptr<udp_send> m_udp_send;
};

} // namespace YDHC
