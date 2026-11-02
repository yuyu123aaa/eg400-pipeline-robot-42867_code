/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#ifndef AL_NPU_EXECUTOR
#define AL_NPU_EXECUTOR

#include <optional>
#include <vector>
#include <string>

#include "armnn/Types.hpp"
#include "armnn/IRuntime.hpp"
#include "armnn/Tensor.hpp"

#include "armnnOnnxParser/IOnnxParser.hpp"
#include "armnnOnnxParser/IOnnxParser.hpp"
#include "flow_common_types.hpp"

/**
 * @brief 综合NPU模型的加载、初始化、推理等功能、输入输出
 *
 */
class NPUExecutor
{
private:
    armnn::IRuntimePtr m_runtime;
    armnn::NetworkId m_netId;

    armnn::InputTensors     m_inputTensors;
    armnn::OutputTensors    m_outputTensors;

    std::vector<armnn::BindingPointInfo> m_inputBindingInfo;
    std::vector<armnn::BindingPointInfo> m_outputBindingInfo;

    std::vector<std::string> m_inputLayerNamesList;
    std::vector<std::string> m_outputLayerNamesList;

	std::vector<CmaBuf> m_inputBuffers;
	std::vector<CmaBuf> m_outputBuffers;

public:
    NPUExecutor() = delete;
    // disbale copy\move constructor
    NPUExecutor(const NPUExecutor &) = delete;
    NPUExecutor(NPUExecutor &&) = delete;
    NPUExecutor &operator=(const NPUExecutor &) = delete;
    NPUExecutor &operator=(NPUExecutor &&) = delete;

    /**
     * @brief Initializes the network with the given input data. Parsed through OnnxParser and optimized for a
     *        given backend.
     *
     * Note that the output layers names order in m_outputLayerNamesList affects the order of the feature vectors
     * in output of the Run method.
     *
     *  @param[in] modelPath - Relative path to the model file. e.g. "models/mymodel.onnx"
     *  @param[in] backends - The list of preferred backends to run inference on
     *  @param[in] network_properties - Some properties for LoadNetwork
     */
    NPUExecutor(const std::string &modelPath, const std::vector<armnn::BackendId> &backends, std::optional<armnn::INetworkProperties> network_properties = {});

    NPUExecutor(
        armnn::INetworkPtr network, std::vector<armnn::BindingPointInfo> inputBindingInfo, std::vector<armnn::BindingPointInfo> outputBindingInfo,
        const std::vector<armnn::BackendId> &backends, std::optional<armnn::INetworkProperties> network_properties = {});

    armnn::DataType GetInputDataType() const;

	int GetInputTensorNum() {return m_inputLayerNamesList.size();}
	int GetOutputTensorNum() {return m_outputLayerNamesList.size();}

	/**
    * @brief Returns the aspect ratio of the associated model in the order of width, height.
    */
    Size GetInputAspectRatio(int tensorIndex);

    float GetInputQuantizationScale(int tensorIndex);
    int GetInputQuantizationOffset(int tensorIndex);

    float GetOutputQuantizationScale(int tensorIndex);
    int GetOutputQuantizationOffset(int tensorIndex);

	std::string GetInputTensorName(int tensorIndex);
	std::string GetOutputTensorName(int tensorIndex);

    /**
     * @brief Get the Input Buffer object
     *
     * @param tensorIndex
     * @return CmaBuf&
     */
    CmaBuf &GetInputBuffer(int tensorIndex);
    /**
     * @brief Get the Output Buffer object
     *
     * @param tensorIndex
     * @return CmaBuf&
     */
    CmaBuf &GetOutputBuffer(int tensorIndex);
    /**
     * @brief Get the Input Tensor object
     *
     * @param tensorIndex
     * @return armnn::ConstTensor&
     */
    armnn::ConstTensor& GetInputTensor(int tensorIndex) {return m_inputTensors.at(tensorIndex).second;}
    /**
     * @brief Get the Output Tensor object
     *
     * @param tensorIndex
     * @return armnn::Tensor&
     */
    armnn::Tensor &GetOutputTensor(int tensorIndex) { return m_outputTensors.at(tensorIndex).second; }
    /**
     * @brief Get the Output Tensors object
     *
     * @return const armnn::OutputTensors&
     */
    const armnn::OutputTensors &GetOutputTensors() { return m_outputTensors; }
    /**
     * @brief Get the Input Tensors object
     *
     * @return const armnn::InputTensors&
     */
    const armnn::InputTensors &GetInputTensors() { return m_inputTensors; }
    
    /**
     * @brief Runs inference on the provided input data.
     *
     * @return true if inference was successful, false otherwise.
     */
    bool Run();

	template <typename Enumeration>
    auto log_as_int(Enumeration value)
    -> typename std::underlying_type<Enumeration>::type
    {
        return static_cast<typename std::underlying_type<Enumeration>::type>(value);
    }

    const armnn::IRuntimePtr &GetRuntime() { return m_runtime; }
    const armnn::NetworkId &GetNetworkId() { return m_netId; }
};

#endif

