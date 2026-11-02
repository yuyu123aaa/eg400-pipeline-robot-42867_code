#ifndef AL_NPU_POSTPROCESS
#define AL_NPU_POSTPROCESS

#include "armnn/Tensor.hpp"
#include "armnnALUtils/DataProcess.hpp"
/**
 * @brief 获取yolo系列模型的后处理函数，包括yolov3 v4 v5。
 *
 * @param outputTensors yolo模型的输出tensor
 * @param input_w 输入图像的宽度
 * @param input_h 输入图像的高度
 * @param stride_anchor_map 模型参数
 * @param class_num 模型参数，总共有几个类别
 * @param prob_threshold 置信度阈值
 * @param nms_threshold 非极大值抑制阈值
 * @return std::function<std::vector<DetectObject>(const armnn::OutputTensors& outputTensors)>
 *         返回的函数需要传入outputTensors参数,返回检测结果
 */
std::function<std::vector<DetectObject>(const armnn::OutputTensors& outputTensors)>
    getFastDecodeOutputYoloWorkLoad(const armnn::OutputTensors& outputTensors, int input_h, int input_w, int cls_num, float prob_threshold, \
            float nms_threshold, const std::unordered_map<int, std::vector<int>>& stride_anchor_map);

/**
 * @brief 获取yolov8模型的后处理函数
 *
 * @param outputTensors yolo模型的输出tensor
 * @param input_w 输入图像的宽度
 * @param input_h 输入图像的高度
 * @param class_num 模型参数，总共有几个类别
 * @param prob_threshold 置信度阈值
 * @param nms_threshold 非极大值抑制阈值
 * @return std::function<std::pair<std::vector<DetectObject>, std::vector<std::vector<std::vector<float>>>>
 *         返回的函数需要传入outputTensors参数,返回检测结果
 */
std::function<std::pair<std::vector<DetectObject>, std::vector<std::vector<std::vector<float>>>>(const armnn::OutputTensors &outputTensors)>
getFastDecodeOutputYoloKpsWorkLoad(
    const armnn::OutputTensors &outputTensors, int input_h, int input_w, int cls_num, float prob_threshold,
    float nms_threshold, const std::unordered_map<int, std::vector<int>> &stride_anchor_map);

/**
 * @brief 获取yolov8模型的后处理函数
 *
 * @param outputTensors yolo模型的输出tensor
 * @param Width 输入图像的宽度
 * @param Height 输入图像的高度
 * @param class_num 模型参数，总共有几个类别
 * @param prob_threshold 置信度阈值
 * @param nms_threshold 非极大值抑制阈值
 * @return std::function<std::vector<DetectObject>(const armnn::OutputTensors& outputTensors)>
 *         返回的函数需要传入outputTensors参数,返回检测结果
 */
std::function<std::vector<DetectObject>(const armnn::OutputTensors &outputTensors)> getFastDecodeOutputYolov8WorkLoad(const armnn::OutputTensors &outputTensors, int input_h, int input_w, int cls_num, float prob_threshold, float nms_threshold);

#endif