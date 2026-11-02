/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "armnnUtils/DataLayoutIndexed.hpp"
#include "executor.hpp"
#include "armnnALUtils/cma_mem_init.hpp"

NPUExecutor::NPUExecutor(const std::string &modelPath, const std::vector<armnn::BackendId> &backends, std::optional<armnn::INetworkProperties> network_properties)
    : m_runtime(armnn::IRuntime::Create(armnn::IRuntime::CreationOptions())) {
    armnn::ConfigureLogging(true, false, armnn::LogSeverity::Error);

    armnnOnnxParser::IOnnxParserPtr parser = armnnOnnxParser::IOnnxParser::Create();

    // Construct ArmNN network
    armnn::INetworkPtr network = parser->CreateNetworkFromBinaryFile(modelPath.c_str());

    // Create ArmNN runtime
    armnn::IRuntime::CreationOptions rt_options; // default options
    rt_options.m_CustomAllocatorMap["CpuAcc"] = rt_options.m_CustomAllocatorMap["CpuRef"] =
        std::shared_ptr<armnn::ICustomAllocator>(new armnn::CmaAllocator);
    m_runtime = armnn::IRuntime::Create(rt_options);

    // Optimise ArmNN network
    armnn::OptimizerOptionsOpaque op_options;
    op_options.SetImportEnabled(true);
    op_options.SetExportEnabled(true);

    armnn::IOptimizedNetworkPtr optNet = armnn::Optimize(*network, backends, m_runtime->GetDeviceSpec(), op_options);

    // Load graph into runtime
    armnn::INetworkProperties networkProperties_default(
        false,
        armnn::MemorySource::Malloc, armnn::MemorySource::Malloc);
    // armnn::MemorySource::Undefined, armnn::MemorySource::Undefined);
    std::string errorMsg;
    m_runtime->LoadNetwork(m_netId, std::move(optNet), errorMsg, network_properties.has_value() ? network_properties.value() : networkProperties_default);

    for (const std::string &name : parser->GetInputTensorNames()) {
        m_inputBindingInfo.push_back(parser->GetNetworkInputBindingInfo(name));
        m_inputLayerNamesList.push_back(name);
    }
    for (const std::string &name : parser->GetOutputTensorNames()) {
        m_outputBindingInfo.push_back(parser->GetNetworkOutputBindingInfo(name));
        m_outputLayerNamesList.push_back(name);
    }

	for (auto &info : m_inputBindingInfo) {
		m_inputBuffers.emplace_back(CmaBuf(info.second.GetNumBytes()));
		info.second.SetConstant(true);
		m_inputTensors.push_back({info.first, armnn::ConstTensor(info.second, m_inputBuffers.back()->virt_addr)});
	}

	for (const auto &info : m_outputBindingInfo) {
		m_outputBuffers.emplace_back(CmaBuf(info.second.GetNumBytes()));
		m_outputTensors.push_back({info.first, armnn::Tensor(info.second, m_outputBuffers.back()->virt_addr)});
	}
}

NPUExecutor::NPUExecutor(
    armnn::INetworkPtr network, std::vector<armnn::BindingPointInfo> inputBindingInfo, std::vector<armnn::BindingPointInfo> outputBindingInfo,
    const std::vector<armnn::BackendId> &backends, std::optional<armnn::INetworkProperties> network_properties)
    : m_runtime(armnn::IRuntime::Create(armnn::IRuntime::CreationOptions())) {
    armnn::ConfigureLogging(true, false, armnn::LogSeverity::Error);

    // Create ArmNN runtime
    armnn::IRuntime::CreationOptions rt_options; // default options
    rt_options.m_CustomAllocatorMap["CpuAcc"] = rt_options.m_CustomAllocatorMap["CpuRef"] =
        std::shared_ptr<armnn::ICustomAllocator>(new armnn::CmaAllocator);
    m_runtime = armnn::IRuntime::Create(rt_options);

    // Optimise ArmNN network
    armnn::OptimizerOptionsOpaque op_options;
    op_options.SetImportEnabled(true);
    op_options.SetExportEnabled(true);

    armnn::IOptimizedNetworkPtr optNet = armnn::Optimize(*network, backends, m_runtime->GetDeviceSpec(), op_options);

    // Load graph into runtime
    armnn::INetworkProperties networkProperties_default(
        false,
        armnn::MemorySource::Malloc, armnn::MemorySource::Malloc);
    // armnn::MemorySource::Undefined, armnn::MemorySource::Undefined);
    std::string errorMsg;
    m_runtime->LoadNetwork(m_netId, std::move(optNet), errorMsg, network_properties.has_value() ? network_properties.value() : networkProperties_default);

    for (const auto &i : inputBindingInfo) {
        m_inputBindingInfo.push_back(i);
        m_inputLayerNamesList.push_back(std::to_string(i.first));
    }
    for (const auto &i : outputBindingInfo) {
        m_outputBindingInfo.push_back(i);
        m_outputLayerNamesList.push_back(std::to_string(i.first));
    }

    for (auto &info : m_inputBindingInfo) {
        m_inputBuffers.emplace_back(CmaBuf(info.second.GetNumBytes()));
        info.second.SetConstant(true);
        m_inputTensors.push_back({info.first, armnn::ConstTensor(info.second, m_inputBuffers.back()->virt_addr)});
    }

    for (const auto &info : m_outputBindingInfo) {
        m_outputBuffers.emplace_back(CmaBuf(info.second.GetNumBytes()));
        m_outputTensors.push_back({info.first, armnn::Tensor(info.second, m_outputBuffers.back()->virt_addr)});
    }
}

Size NPUExecutor::GetInputAspectRatio(int tensorIndex) {
	const auto shape = m_inputBindingInfo.at(tensorIndex).second.GetShape();
    assert(shape.GetNumDimensions() == 4);
    armnnUtils::DataLayoutIndexed nchw(armnn::DataLayout::NCHW);
    return Size(shape[nchw.GetWidthIndex()],
                shape[nchw.GetHeightIndex()]);
}

armnn::DataType NPUExecutor::GetInputDataType() const {
	assert(m_inputLayerNamesList.size() > 0);
	return m_inputBindingInfo.at(0).second.GetDataType();
}

float NPUExecutor::GetInputQuantizationScale(int tensorIndex) {
	return m_inputBindingInfo.at(tensorIndex).second.GetQuantizationScale();
}

int NPUExecutor::GetInputQuantizationOffset(int tensorIndex) {
	return m_inputBindingInfo.at(tensorIndex).second.GetQuantizationOffset();
}

float NPUExecutor::GetOutputQuantizationScale(int tensorIndex) {
	return m_outputBindingInfo.at(tensorIndex).second.GetQuantizationScale();
}

int NPUExecutor::GetOutputQuantizationOffset(int tensorIndex) {
	return m_outputBindingInfo.at(tensorIndex).second.GetQuantizationOffset();
}

std::string NPUExecutor::GetInputTensorName(int tensorIndex) {
	return m_inputLayerNamesList.at(tensorIndex);
}

std::string NPUExecutor::GetOutputTensorName(int tensorIndex) {
	return m_outputLayerNamesList.at(tensorIndex);
}

CmaBuf& NPUExecutor::GetInputBuffer(int tensorIndex) {
	return m_inputBuffers.at(tensorIndex);
}

CmaBuf& NPUExecutor::GetOutputBuffer(int tensorIndex) {
	return m_outputBuffers.at(tensorIndex);
}

bool NPUExecutor::Run() {

    armnn::Status ret = m_runtime->EnqueueWorkload(m_netId, m_inputTensors, m_outputTensors);

    std::stringstream inferenceFinished;
    inferenceFinished << "Inference finished with code {" << log_as_int(ret) << "}\n";

    ARMNN_LOG(trace) << inferenceFinished.str();

    if (ret == armnn::Status::Failure)
    {
        ARMNN_LOG(error) << "Failed to perform inference.";
    }

    return (armnn::Status::Success == ret);
}
