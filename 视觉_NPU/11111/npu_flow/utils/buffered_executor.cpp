/****************************************************************
 * Copyright (c) 2023 Anlogic, Inc.  All rights reserved.
 * SPDX-License-Identifier: MIT
 ****************************************************************/

#include "armnnUtils/DataLayoutIndexed.hpp"
#include "armnnALUtils/cma_mem_init.hpp"
#include "buffered_executor.hpp"
#include <armnn/IStrategy.hpp>
#include <cstdio>

namespace {
void debug_stage(const char *message) {
    std::fprintf(stderr, "[MODEL-DEBUG] %s\n", message);
    std::fflush(stderr);
}

#ifdef FORCE_CONCAT_CPUACC
class ForceConcatCpuAccStrategy final : public armnn::IStrategy {
public:
    ~ForceConcatCpuAccStrategy() override = default;

    void ExecuteStrategy(const armnn::IConnectableLayer *layer,
                         const armnn::BaseDescriptor &descriptor,
                         const std::vector<armnn::ConstTensor> &constants,
                         const char *name,
                         const armnn::LayerBindingId id = 0) override {
        (void)descriptor;
        (void)constants;
        (void)name;
        (void)id;
        if (layer->GetType() == armnn::LayerType::Concat) {
            auto *mutableLayer = const_cast<armnn::IConnectableLayer *>(layer);
            mutableLayer->BackendSelectionHint(armnn::BackendId("CpuAcc"));
            ++m_ConcatCount;
        }
    }

    unsigned int GetConcatCount() const { return m_ConcatCount; }

private:
    unsigned int m_ConcatCount = 0;
};
#endif

#ifdef FORCE_SOFTOPS_CPUACC
class ForceSoftOpsCpuAccStrategy final : public armnn::IStrategy {
public:
    ~ForceSoftOpsCpuAccStrategy() override = default;

    void ExecuteStrategy(const armnn::IConnectableLayer *layer,
                         const armnn::BaseDescriptor &descriptor,
                         const std::vector<armnn::ConstTensor> &constants,
                         const char *name,
                         const armnn::LayerBindingId id = 0) override {
        (void)descriptor;
        (void)constants;
        (void)name;
        (void)id;

        bool forceCpuAcc = false;
        switch (layer->GetType()) {
        case armnn::LayerType::Concat:
            ++m_ConcatCount;
            forceCpuAcc = true;
            break;
        case armnn::LayerType::Pooling2d:
            ++m_PoolingCount;
            forceCpuAcc = true;
            break;
        case armnn::LayerType::Resize:
            ++m_ResizeCount;
            forceCpuAcc = true;
            break;
        case armnn::LayerType::ElementwiseBinary:
            ++m_ElementwiseCount;
            forceCpuAcc = true;
            break;
        default:
            break;
        }

        if (forceCpuAcc) {
            auto *mutableLayer = const_cast<armnn::IConnectableLayer *>(layer);
            mutableLayer->BackendSelectionHint(armnn::BackendId("CpuAcc"));
        }
    }

    unsigned int GetConcatCount() const { return m_ConcatCount; }
    unsigned int GetPoolingCount() const { return m_PoolingCount; }
    unsigned int GetResizeCount() const { return m_ResizeCount; }
    unsigned int GetElementwiseCount() const { return m_ElementwiseCount; }

private:
    unsigned int m_ConcatCount = 0;
    unsigned int m_PoolingCount = 0;
    unsigned int m_ResizeCount = 0;
    unsigned int m_ElementwiseCount = 0;
};
#endif
}



BufferedNPUExecutor::BufferedNPUExecutor(std::string& modelPath, std::vector<armnn::BackendId>& backends): m_runtime(armnn::IRuntime::Create(armnn::IRuntime::CreationOptions())) {
    debug_stage("01 constructor entered; initial runtime created");
    armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);

    debug_stage("02 creating ONNX parser");
    armnnOnnxParser::IOnnxParserPtr parser = armnnOnnxParser::IOnnxParser::Create();

    // Construct ArmNN network
    debug_stage("03 parsing ONNX file: begin");
    armnn::INetworkPtr network = parser->CreateNetworkFromBinaryFile(modelPath.c_str());
    debug_stage("04 parsing ONNX file: returned");
    if (!network) {
        throw std::runtime_error("ONNX parser returned a null network");
    }

#ifdef FORCE_SOFTOPS_CPUACC
    ForceSoftOpsCpuAccStrategy softOpsStrategy;
    network->ExecuteStrategy(softOpsStrategy);
    std::fprintf(stderr,
                 "[MODEL-DEBUG] forced soft ops to prefer CpuAcc: Concat=%u Pooling2d=%u Resize=%u ElementwiseBinary=%u\n",
                 softOpsStrategy.GetConcatCount(),
                 softOpsStrategy.GetPoolingCount(),
                 softOpsStrategy.GetResizeCount(),
                 softOpsStrategy.GetElementwiseCount());
    std::fflush(stderr);
#elif defined(FORCE_CONCAT_CPUACC)
    ForceConcatCpuAccStrategy concatStrategy;
    network->ExecuteStrategy(concatStrategy);
    std::fprintf(stderr,
                 "[MODEL-DEBUG] forced %u Concat layers to prefer CpuAcc\n",
                 concatStrategy.GetConcatCount());
    std::fflush(stderr);
#endif

    // Create ArmNN runtime
    debug_stage("05 creating configured ArmNN runtime: begin");
    armnn::IRuntime::CreationOptions rt_options; // default options
    rt_options.m_CustomAllocatorMap["CpuAcc"] = rt_options.m_CustomAllocatorMap["CpuRef"] =
        std::shared_ptr<armnn::ICustomAllocator>(new armnn::CmaAllocator);
    m_runtime = armnn::IRuntime::Create(rt_options);
    debug_stage("06 creating configured ArmNN runtime: returned");
    if (!m_runtime) {
        throw std::runtime_error("ArmNN runtime creation returned null");
    }

    // Optimise ArmNN network
    armnn::OptimizerOptionsOpaque op_options;
    op_options.SetImportEnabled(true);
    op_options.SetExportEnabled(true);

    debug_stage("07 Alnpu Optimize: begin");
    armnn::IOptimizedNetworkPtr optNet = armnn::Optimize(*network, backends, m_runtime->GetDeviceSpec(), op_options);
    debug_stage("08 Alnpu Optimize: returned");
    if (!optNet) {
        throw std::runtime_error("ArmNN Optimize returned a null network");
    }

    // Load graph into runtime
    armnn::INetworkProperties networkProperties(
        false,
        armnn::MemorySource::Malloc, armnn::MemorySource::Malloc,
        false,
        armnn::ProfilingDetailsMethod::DetailsWithEvents, false, 2);
    std::string errorMsg;
    debug_stage("09 LoadNetwork: begin");
    const armnn::Status loadStatus = m_runtime->LoadNetwork(m_netId, std::move(optNet), errorMsg, networkProperties);
    debug_stage("10 LoadNetwork: returned");
    if (loadStatus != armnn::Status::Success) {
        throw std::runtime_error("LoadNetwork failed: " + errorMsg);
    }

    armnn::ConfigureLogging(true, false, armnn::LogSeverity::Error);

    debug_stage("11 reading input/output binding information");
    for (const std::string &name : parser->GetInputTensorNames()) {
        m_inputBindingInfo.push_back(parser->GetNetworkInputBindingInfo(name));
        m_inputLayerNamesList.push_back(name);
    }
    for (const std::string &name : parser->GetOutputTensorNames()) {
        m_outputBindingInfo.push_back(parser->GetNetworkOutputBindingInfo(name));
        m_outputLayerNamesList.push_back(name);
    }

    
    debug_stage("12 allocating CMA input/output buffers");
    for (auto &info : m_inputBindingInfo) {
        info.second.SetConstant(true);
        for (int idx = 0; idx < MAX_BUF_NUM; idx++) {
            m_inputBuffers.at(idx).emplace_back(CmaBuf(info.second.GetNumBytes()));
            m_inputTensors.at(idx).push_back({info.first, armnn::ConstTensor(info.second, m_inputBuffers.at(idx).back()->virt_addr)});
        }
	}
    
	for (const auto &info : m_outputBindingInfo) {
        for (int idx = 0; idx < MAX_BUF_NUM; idx++) {
            m_outputBuffers.at(idx).emplace_back(CmaBuf(info.second.GetNumBytes()));
            m_outputTensors.at(idx).push_back({info.first, armnn::Tensor(info.second, m_outputBuffers.at(idx).back()->virt_addr)});
        }
	}
    debug_stage("13 BufferedNPUExecutor construction complete");
}

Size BufferedNPUExecutor::GetInputAspectRatio(int tensorIndex) {
	const auto shape = m_inputBindingInfo.at(tensorIndex).second.GetShape();
    assert(shape.GetNumDimensions() == 4);
    armnnUtils::DataLayoutIndexed nchw(armnn::DataLayout::NCHW);
    return Size(shape[nchw.GetWidthIndex()],
                shape[nchw.GetHeightIndex()]);
}

armnn::DataType BufferedNPUExecutor::GetInputDataType() const {
	assert(m_inputLayerNamesList.size() > 0);
	return m_inputBindingInfo.at(0).second.GetDataType();
}

float BufferedNPUExecutor::GetInputQuantizationScale(int tensorIndex) {
	return m_inputBindingInfo.at(tensorIndex).second.GetQuantizationScale();
}

int BufferedNPUExecutor::GetInputQuantizationOffset(int tensorIndex) {
	return m_inputBindingInfo.at(tensorIndex).second.GetQuantizationOffset();
}

float BufferedNPUExecutor::GetOutputQuantizationScale(int tensorIndex) {
	return m_outputBindingInfo.at(tensorIndex).second.GetQuantizationScale();
}

int BufferedNPUExecutor::GetOutputQuantizationOffset(int tensorIndex) {
	return m_outputBindingInfo.at(tensorIndex).second.GetQuantizationOffset();
}

std::string BufferedNPUExecutor::GetInputTensorName(int tensorIndex) {
	return m_inputLayerNamesList.at(tensorIndex);
}

std::string BufferedNPUExecutor::GetOutputTensorName(int tensorIndex) {
	return m_outputLayerNamesList.at(tensorIndex);
}

CmaBuf& BufferedNPUExecutor::GetInputBuffer(int tensorIndex, int bufferIndex) {
	return m_inputBuffers.at(bufferIndex).at(tensorIndex);
}

CmaBuf& BufferedNPUExecutor::GetOutputBuffer(int tensorIndex, int bufferIndex) {
	return m_outputBuffers.at(bufferIndex).at(tensorIndex);
}

bool BufferedNPUExecutor::Run(int bufferIndex) {

    armnn::Status ret = m_runtime->EnqueueWorkload(m_netId, m_inputTensors.at(bufferIndex), m_outputTensors.at(bufferIndex));

    std::stringstream inferenceFinished;
    inferenceFinished << "Inference finished with code {" << log_as_int(ret) << "}\n";

    ARMNN_LOG(trace) << inferenceFinished.str();

    if (ret == armnn::Status::Failure)
    {
        ARMNN_LOG(error) << "Failed to perform inference.";
    }

    return (armnn::Status::Success == ret);
}


BufferedNPUExecutorMultiModel::BufferedNPUExecutorMultiModel(std::vector<std::string>& modelPaths, std::vector<armnn::BackendId>& backends): m_runtime(armnn::IRuntime::Create(armnn::IRuntime::CreationOptions())) {
    armnn::ConfigureLogging(true, false, armnn::LogSeverity::Info);

    armnnOnnxParser::IOnnxParserPtr parser = armnnOnnxParser::IOnnxParser::Create();

    // Create ArmNN runtime
    armnn::IRuntime::CreationOptions rt_options; // default options
    rt_options.m_CustomAllocatorMap["CpuAcc"] = rt_options.m_CustomAllocatorMap["CpuRef"] =
        std::shared_ptr<armnn::ICustomAllocator>(new armnn::CmaAllocator);
    m_runtime = armnn::IRuntime::Create(rt_options);

    // Optimise ArmNN network
    armnn::OptimizerOptionsOpaque op_options;
    op_options.SetImportEnabled(true);
    op_options.SetExportEnabled(true);

    armnn::INetworkProperties networkProperties(
        false,
        armnn::MemorySource::Malloc, armnn::MemorySource::Malloc,
        false,
        armnn::ProfilingDetailsMethod::DetailsWithEvents, false, 2);

    armnn::ConfigureLogging(true, false, armnn::LogSeverity::Error);


    m_netIds.resize(modelPaths.size());
    m_inputBindingInfos.resize(modelPaths.size());
    m_inputLayerNamesLists.resize(modelPaths.size());
    m_inputBuffers.resize(modelPaths.size());
    m_inputTensors.resize(modelPaths.size());
    m_outputBindingInfos.resize(modelPaths.size());
    m_outputLayerNamesLists.resize(modelPaths.size());
    m_outputBuffers.resize(modelPaths.size());
    m_outputTensors.resize(modelPaths.size());

    // Construct ArmNN network 
    for(int modelIndex = 0; modelIndex < modelPaths.size(); modelIndex++) {
        armnn::INetworkPtr network = parser->CreateNetworkFromBinaryFile(modelPaths[modelIndex].c_str());
        armnn::IOptimizedNetworkPtr optNet = armnn::Optimize(*network, backends, m_runtime->GetDeviceSpec(), op_options);

        // Load graph into runtime
        std::string errorMsg;
        m_runtime->LoadNetwork(m_netIds[modelIndex], std::move(optNet), errorMsg, networkProperties);

        for (const std::string &name : parser->GetInputTensorNames()) {
            m_inputBindingInfos[modelIndex].push_back(parser->GetNetworkInputBindingInfo(name));
            m_inputLayerNamesLists[modelIndex].push_back(name);
        }
        for (const std::string &name : parser->GetOutputTensorNames()) {
            m_outputBindingInfos[modelIndex].push_back(parser->GetNetworkOutputBindingInfo(name));
            m_outputLayerNamesLists[modelIndex].push_back(name);
        }

        for (auto &info : m_inputBindingInfos[modelIndex]) {
            info.second.SetConstant(true);
            for (int idx = 0; idx < MAX_BUF_NUM; idx++) {
                m_inputBuffers[modelIndex].at(idx).emplace_back(CmaBuf(info.second.GetNumBytes()));
                m_inputTensors[modelIndex].at(idx).push_back({info.first, armnn::ConstTensor(info.second, m_inputBuffers[modelIndex].at(idx).back()->virt_addr)});
            }
        }
        
        for (const auto &info : m_outputBindingInfos[modelIndex]) {
            for (int idx = 0; idx < MAX_BUF_NUM; idx++) {
                m_outputBuffers[modelIndex].at(idx).emplace_back(CmaBuf(info.second.GetNumBytes()));
                m_outputTensors[modelIndex].at(idx).push_back({info.first, armnn::Tensor(info.second, m_outputBuffers[modelIndex].at(idx).back()->virt_addr)});
            }
        }
    }
           
    

}

Size BufferedNPUExecutorMultiModel::GetInputAspectRatio(int modelIndex, int tensorIndex) {
	const auto shape = m_inputBindingInfos[modelIndex].at(tensorIndex).second.GetShape();
    assert(shape.GetNumDimensions() == 4);
    armnnUtils::DataLayoutIndexed nchw(armnn::DataLayout::NCHW);
    return Size(shape[nchw.GetWidthIndex()],
                shape[nchw.GetHeightIndex()]);
}

armnn::DataType BufferedNPUExecutorMultiModel::GetInputDataType(int modelIndex) const {
	assert(m_inputLayerNamesLists[modelIndex].size() > 0);
	return m_inputBindingInfos[modelIndex].at(0).second.GetDataType();
}

float BufferedNPUExecutorMultiModel::GetInputQuantizationScale(int modelIndex, int tensorIndex) {
	return m_inputBindingInfos[modelIndex].at(tensorIndex).second.GetQuantizationScale();
}

int BufferedNPUExecutorMultiModel::GetInputQuantizationOffset(int modelIndex, int tensorIndex) {
	return m_inputBindingInfos[modelIndex].at(tensorIndex).second.GetQuantizationOffset();
}

float BufferedNPUExecutorMultiModel::GetOutputQuantizationScale(int modelIndex, int tensorIndex) {
	return m_outputBindingInfos[modelIndex].at(tensorIndex).second.GetQuantizationScale();
}

int BufferedNPUExecutorMultiModel::GetOutputQuantizationOffset(int modelIndex, int tensorIndex) {
	return m_outputBindingInfos[modelIndex].at(tensorIndex).second.GetQuantizationOffset();
}

std::string BufferedNPUExecutorMultiModel::GetInputTensorName(int modelIndex, int tensorIndex) {
	return m_inputLayerNamesLists[modelIndex].at(tensorIndex);
}

std::string BufferedNPUExecutorMultiModel::GetOutputTensorName(int modelIndex, int tensorIndex) {
	return m_outputLayerNamesLists[modelIndex].at(tensorIndex);
}

CmaBuf& BufferedNPUExecutorMultiModel::GetInputBuffer(int modelIndex, int tensorIndex, int bufferIndex) {
	return m_inputBuffers[modelIndex].at(bufferIndex).at(tensorIndex);
}

CmaBuf& BufferedNPUExecutorMultiModel::GetOutputBuffer(int modelIndex, int tensorIndex, int bufferIndex) {
	return m_outputBuffers[modelIndex].at(bufferIndex).at(tensorIndex);
}

bool BufferedNPUExecutorMultiModel::Run(int modelIndex, int bufferIndex) {

    armnn::Status ret = m_runtime->EnqueueWorkload(m_netIds[modelIndex], m_inputTensors[modelIndex].at(bufferIndex), m_outputTensors[modelIndex].at(bufferIndex));

    std::stringstream inferenceFinished;
    inferenceFinished << "Inference finished with code {" << log_as_int(ret) << "}\n";

    ARMNN_LOG(trace) << inferenceFinished.str();

    if (ret == armnn::Status::Failure)
    {
        ARMNN_LOG(error) << "Failed to perform inference.";
    }

    return (armnn::Status::Success == ret);
}
