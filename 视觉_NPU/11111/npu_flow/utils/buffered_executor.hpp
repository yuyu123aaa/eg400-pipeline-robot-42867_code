/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#ifndef AL_BUFFERED_NPU_EXECUTOR
#define AL_BUFFERED_NPU_EXECUTOR

#include <vector>
#include <string>

#include "armnn/Types.hpp"
#include "armnn/IRuntime.hpp"
#include "armnn/Tensor.hpp"

#include "armnnOnnxParser/IOnnxParser.hpp"
#include "armnnOnnxParser/IOnnxParser.hpp"

#include "flow_common_types.hpp"

#define MAX_BUF_NUM 8

class BufferedNPUExecutor
{
#ifndef MAX_BUF_NUM
#define MAX_BUF_NUM 8
#endif

private:
    armnn::IRuntimePtr m_runtime;
    armnn::NetworkId m_netId;

    std::array<armnn::InputTensors, MAX_BUF_NUM>   m_inputTensors;
    std::array<armnn::OutputTensors, MAX_BUF_NUM>  m_outputTensors;

    std::vector<armnnOnnxParser::BindingPointInfo> m_inputBindingInfo;
    std::vector<armnnOnnxParser::BindingPointInfo> m_outputBindingInfo;

    std::vector<std::string> m_inputLayerNamesList;
    std::vector<std::string> m_outputLayerNamesList;

	std::array<std::vector<CmaBuf>, MAX_BUF_NUM> m_inputBuffers;
	std::array<std::vector<CmaBuf>, MAX_BUF_NUM> m_outputBuffers;

public:
    // NPUExecutor() = delete;
     BufferedNPUExecutor(): m_runtime(armnn::IRuntime::Create(armnn::IRuntime::CreationOptions())) {}

    /**
    * @brief Initializes the network with the given input data. Parsed through TfLiteParser and optimized for a
    *        given backend.
    *
    * Note that the output layers names order in m_outputLayerNamesList affects the order of the feature vectors
    * in output of the Run method.
    *
    *       * @param[in] modelPath - Relative path to the model file
    *       * @param[in] backends - The list of preferred backends to run inference on
    */
    BufferedNPUExecutor(std::string& modelPath, std::vector<armnn::BackendId>& backends);


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

	CmaBuf& GetInputBuffer(int tensorIndex, int bufferIndex);
	CmaBuf& GetOutputBuffer(int tensorIndex, int bufferIndex);

    armnn::ConstTensor& GetInputTensor(int tensorIndex, int bufferIndex) {return m_inputTensors.at(bufferIndex).at(tensorIndex).second;}
	const armnn::OutputTensors& GetOutputTensors(int bufferIndex) {return m_outputTensors.at(bufferIndex);}

    /**
    * @brief Runs inference on the provided input data, and stores the results in the provided InferenceResults object.
    *
    * @param[out] results - Vector of DetectionResult objects used to store the output result.
    */
    bool Run(int bufferIndex);

	template <typename Enumeration>
    auto log_as_int(Enumeration value)
    -> typename std::underlying_type<Enumeration>::type
    {
        return static_cast<typename std::underlying_type<Enumeration>::type>(value);
    }

};

class BufferedNPUExecutorMultiModel
{
#ifndef MAX_BUF_NUM
#define MAX_BUF_NUM 8
#endif

private:
    armnn::IRuntimePtr m_runtime;
    std::vector<armnn::NetworkId> m_netIds;

    std::vector<std::array<armnn::InputTensors, MAX_BUF_NUM>>   m_inputTensors;
    std::vector<std::array<armnn::OutputTensors, MAX_BUF_NUM>>  m_outputTensors;

    std::vector<std::vector<armnnOnnxParser::BindingPointInfo>> m_inputBindingInfos;
    std::vector<std::vector<armnnOnnxParser::BindingPointInfo>> m_outputBindingInfos;

    std::vector<std::vector<std::string>> m_inputLayerNamesLists;
    std::vector<std::vector<std::string>> m_outputLayerNamesLists;

	std::vector<std::array<std::vector<CmaBuf>, MAX_BUF_NUM>> m_inputBuffers;
	std::vector<std::array<std::vector<CmaBuf>, MAX_BUF_NUM>> m_outputBuffers;

public:
    // NPUExecutor() = delete;
    BufferedNPUExecutorMultiModel(): m_runtime(armnn::IRuntime::Create(armnn::IRuntime::CreationOptions())) {}

    /**
    * @brief Initializes the network with the given input data. Parsed through TfLiteParser and optimized for a
    *        given backend.
    *
    * Note that the output layers names order in m_outputLayerNamesList affects the order of the feature vectors
    * in output of the Run method.
    *
    *       * @param[in] modelPath - Relative path to the model file
    *       * @param[in] backends - The list of preferred backends to run inference on
    */
   BufferedNPUExecutorMultiModel(std::vector<std::string>& modelPaths, std::vector<armnn::BackendId>& backends);


    armnn::DataType GetInputDataType(int modelIndex) const;

	int GetInputTensorNum(int modelIndex) {return m_inputLayerNamesLists[modelIndex].size();}
	int GetOutputTensorNum(int modelIndex) {return m_outputLayerNamesLists[modelIndex].size();}

	/**
    * @brief Returns the aspect ratio of the associated model in the order of width, height.
    */
    Size GetInputAspectRatio(int modelIndex, int tensorIndex);

    float GetInputQuantizationScale(int modelIndex, int tensorIndex);
    int GetInputQuantizationOffset(int modelIndex, int tensorIndex);

    float GetOutputQuantizationScale(int modelIndex, int tensorIndex);
    int GetOutputQuantizationOffset(int modelIndex, int tensorIndex);

	std::string GetInputTensorName(int modelIndex, int tensorIndex);
	std::string GetOutputTensorName(int modelIndex, int tensorIndex);

	CmaBuf& GetInputBuffer(int modelIndex, int tensorIndex, int bufferIndex);
	CmaBuf& GetOutputBuffer(int modelIndex, int tensorIndex, int bufferIndex);

    armnn::ConstTensor& GetInputTensor(int modelIndex, int tensorIndex, int bufferIndex) {return m_inputTensors[modelIndex].at(bufferIndex).at(tensorIndex).second;}
	const armnn::OutputTensors& GetOutputTensors(int modelIndex, int bufferIndex) {return m_outputTensors[modelIndex].at(bufferIndex);}

    /**
    * @brief Runs inference on the provided input data, and stores the results in the provided InferenceResults object.
    *
    * @param[out] results - Vector of DetectionResult objects used to store the output result.
    */
    bool Run(int modelIndex, int bufferIndex);

	template <typename Enumeration>
    auto log_as_int(Enumeration value)
    -> typename std::underlying_type<Enumeration>::type
    {
        return static_cast<typename std::underlying_type<Enumeration>::type>(value);
    }

};

#endif

