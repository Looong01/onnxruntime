// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "qnn_model.h"

#include <iostream>
#include <fstream>
#include <gsl/gsl>
#include "QnnOpDef.h"

#include "core/providers/qnn/builder/op_builder_factory.h"
#include "core/providers/qnn/builder/qnn_node_group/qnn_node_group.h"
#include "core/providers/qnn/builder/qnn_utils.h"
#include "core/providers/qnn/ort_api.h"
#include "core/providers/qnn/qnn_allocator.h"
#include "core/providers/qnn/shared_context.h"

namespace onnxruntime {
namespace qnn {

bool QnnModel::GetGraphInfoFromModel(QnnModelWrapper& model_wrapper, const logging::Logger& /* logger */) {
  bool rt = true;

  graph_info_ = std::make_unique<GraphInfo>(model_wrapper.GetQnnGraph(),
                                            model_wrapper.GetQnnGraphName(),
                                            model_wrapper.GetQnnGraphContext(),
                                            std::move(model_wrapper.GetGraphInputTensorWrappers()),
                                            std::move(model_wrapper.GetGraphOutputTensorWrappers()));

  return rt;
}

Status QnnModel::SetGraphInputOutputInfo(const GraphViewer& graph_viewer,
                                         const onnxruntime::Node& fused_node,
                                         const logging::Logger& logger) {
  auto input_defs = fused_node.InputDefs();
  ORT_RETURN_IF_ERROR(ParseGraphInputOrOutput(graph_viewer, input_defs, input_names_, inputs_info_,
                                              model_input_index_map_, logger, true));

  auto output_defs = fused_node.OutputDefs();
  ORT_RETURN_IF_ERROR(ParseGraphInputOrOutput(graph_viewer, output_defs, output_names_, outputs_info_,
                                              model_output_index_map_, logger));

  return Status::OK();
}

Status QnnModel::ParseGraphInputOrOutput(const GraphViewer& graph_viewer,
                                         ConstPointerContainer<std::vector<NodeArg*>>& input_output_defs,
                                         std::vector<std::string>& input_output_names,
                                         std::unordered_map<std::string, OnnxTensorInfo>& input_output_info_table,
                                         std::unordered_map<std::string, size_t>& input_output_index_map,
                                         const logging::Logger& logger,
                                         bool is_input) {
  for (size_t i = 0, end = input_output_defs.size(), index = 0; i < end; ++i) {
    const auto& name = input_output_defs[i]->Name();
    if (is_input) {
      if (graph_viewer.IsConstantInitializer(name, true)) {
        continue;  // exclude initializer inputs
      }
    }
    // Validate input/output shape
    LOGS(logger, VERBOSE) << (is_input ? "input " : "output ") << i << " " << name;
    input_output_index_map.emplace(name, index++);
    const auto* shape_proto = input_output_defs[i]->Shape();  // consider use qnn_model_wrapper.GetOnnxShape
    ORT_RETURN_IF(shape_proto == nullptr, "shape_proto cannot be null for output: ", name);

    const auto& dims = shape_proto->dim();
    std::vector<int64_t> shape;
    shape.reserve(dims.size());
    for (const auto& dim : dims) {
      ORT_RETURN_IF_NOT(dim.has_dim_value(), "Dynamic shape is not supported yet, for output: ", name);
      shape.push_back(dim.dim_value());
    }
    const auto* type_proto = input_output_defs[i]->TypeAsProto();
    int32_t data_type = type_proto->tensor_type().elem_type();
    // use index i so that for graph input, it has initializers included
    input_output_info_table.emplace(std::piecewise_construct, std::forward_as_tuple(name), std::forward_as_tuple(i, data_type, std::move(shape)));
    input_output_names.push_back(name);
  }

  return Status::OK();
}

const NodeUnit& QnnModel::GetNodeUnit(const Node* node,
                                      const std::unordered_map<const Node*, const NodeUnit*>& node_unit_map) const {
  const auto node_unit_it = node_unit_map.find(node);
  ORT_ENFORCE(node_unit_it != node_unit_map.end(), "Node does not have corresponding NodeUnit.");
  return *node_unit_it->second;
}

Status QnnModel::ComposeGraph(const GraphViewer& graph_viewer,
                              const onnxruntime::Node& fused_node,
                              const qnn::ModelSettings& model_settings,
                              const logging::Logger& logger,
                              const QnnGraph_Config_t** graph_configs,
                              const std::string& json_qnn_graph_path) {
  LOGS(logger, VERBOSE) << "ComposeGraph Graph name: " << graph_viewer.Name();

  // Holder for the NodeUnits in the graph, this will guarantee the NodeUnits is
  // valid throughout the lifetime of the ModelBuilder
  std::vector<std::unique_ptr<NodeUnit>> node_unit_holder;
  std::unordered_map<const Node*, const NodeUnit*> node_unit_map;
  std::tie(node_unit_holder, node_unit_map) = GetQDQNodeUnits(graph_viewer, logger);

  // This name must be same with the EPContext node name
  const auto& graph_name = fused_node.Name();
  ORT_RETURN_IF_ERROR(SetGraphInputOutputInfo(graph_viewer, fused_node, logger));

  QnnModelWrapper qnn_model_wrapper = QnnModelWrapper(graph_viewer, logger,
                                                      qnn_backend_manager_->GetQnnInterface(),
                                                      qnn_backend_manager_->GetQnnBackendHandle(),
                                                      model_input_index_map_,
                                                      model_output_index_map_,
                                                      qnn_backend_manager_->GetQnnBackendType(),
                                                      model_settings);
  bool rt = true;
  rt = qnn_model_wrapper.CreateQnnGraph(qnn_backend_manager_->GetQnnContext(), graph_name, graph_configs);
  if (!rt) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Failed to initialize qnn_model_wrapper.");
  }

  std::vector<std::unique_ptr<qnn::IQnnNodeGroup>> qnn_node_groups;
  qnn_node_groups.reserve(node_unit_holder.size());

  ORT_RETURN_IF_ERROR(qnn::GetQnnNodeGroups(qnn_node_groups, qnn_model_wrapper, node_unit_map,
                                            node_unit_holder.size(), logger));

  for (const std::unique_ptr<qnn::IQnnNodeGroup>& qnn_node_group : qnn_node_groups) {
    Status status = qnn_node_group->AddToModelBuilder(qnn_model_wrapper, logger);

    if (!status.IsOK()) {
      LOGS(logger, ERROR) << "[QNN EP] Failed to add supported node to QNN graph during EP's compile call: "
                          << status.ErrorMessage() << std::endl;
      return status;
    }
  }

  const bool build_json_graph = !json_qnn_graph_path.empty();
  ORT_RETURN_IF_NOT(qnn_model_wrapper.ComposeQnnGraph(build_json_graph), "Failed to compose Qnn graph.");

  if (build_json_graph) {
    const nlohmann::json& json_graph = qnn_model_wrapper.GetQnnJSONGraph();
    std::ofstream ofs(json_qnn_graph_path);

    if (ofs.is_open()) {
      ofs << json_graph.dump();
      ofs.close();
    } else {
      LOGS(logger, WARNING) << "Could not open JSON graph file: " << json_qnn_graph_path;
    }
  }

  rt = GetGraphInfoFromModel(qnn_model_wrapper, logger);
  if (!rt) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "GetGraphInfoFromModel failed.");
  }
  LOGS(logger, VERBOSE) << "GetGraphInfoFromModel completed.";
  return Status::OK();
}

Status QnnModel::FinalizeGraphs(const logging::Logger& logger) {
  LOGS(logger, VERBOSE) << "FinalizeGraphs started.";
  Qnn_ErrorHandle_t status = qnn_backend_manager_->GetQnnInterface().graphFinalize(graph_info_->Graph(),
                                                                                   qnn_backend_manager_->GetQnnProfileHandle(),
                                                                                   nullptr);
  if (QNN_GRAPH_NO_ERROR != status) {
    LOGS(logger, ERROR) << "Failed to finalize QNN graph. Error code: " << status;
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Failed to finalize QNN graph.");
  }

  ORT_RETURN_IF_ERROR(qnn_backend_manager_->ExtractBackendProfilingInfo());

  LOGS(logger, VERBOSE) << "FinalizeGraphs completed.";
  return Status::OK();
}

Status QnnModel::SetupQnnInputOutput(const logging::Logger& logger) {
  LOGS(logger, VERBOSE) << "Setting up QNN input/output for graph: " << graph_info_->Name();

  auto result = SetupTensors(qnn_input_infos_, graph_info_->InputTensors());

  if (Status::OK() != result) {
    const std::string message = "Failed to setup QNN input tensors for graph: " + graph_info_->Name();
    LOGS(logger, ERROR) << message;
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, message);
  }

  result = SetupTensors(qnn_output_infos_, graph_info_->OutputTensors(), false);
  if (Status::OK() != result) {
    const std::string message = "Failed to setup QNN output tensors for graph: " + graph_info_->Name();
    LOGS(logger, ERROR) << message;
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, message);
  }

  return Status::OK();
}

static Status BindQnnTensorMemoryToOrtValueMemory(const logging::Logger& logger,
                                                  QnnBackendManager& qnn_backend_manager,
                                                  const OrtMemoryInfo& ort_value_memory_info,
                                                  void* ort_value_data, uint32_t ort_value_data_size,
                                                  Qnn_ContextHandle_t qnn_context,
                                                  Qnn_Tensor_t& qnn_tensor) {
  // either set qnn_tensor memHandle or clientBuf
  const static auto htp_shared_mem_info = HtpSharedMemoryAllocator::AssociatedMemoryInfo();
  const bool uses_shared_memory = (ort_value_memory_info.device.Type() == htp_shared_mem_info.device.Type() &&
                                   ort_value_memory_info.device.MemType() == htp_shared_mem_info.device.MemType());

  if (!uses_shared_memory) {
    LOGS(logger, VERBOSE) << "Setting Qnn_Tensor_t clientBuf to ORT tensor memory.";
    SetQnnTensorMemType(qnn_tensor, QNN_TENSORMEMTYPE_RAW);
    SetQnnTensorClientBuf(qnn_tensor, ort_value_data, ort_value_data_size);
  } else {
    LOGS(logger, VERBOSE) << "Setting Qnn_Tensor_t memHandle to ORT tensor shared memory.";
    Qnn_MemHandle_t qnn_mem_handle{};
    ORT_RETURN_IF_ERROR(qnn_backend_manager.GetOrRegisterContextMemHandle(qnn_context, ort_value_data, qnn_tensor,
                                                                          qnn_mem_handle));
    SetQnnTensorMemType(qnn_tensor, QNN_TENSORMEMTYPE_MEMHANDLE);
    SetQnnTensorMemHandle(qnn_tensor, qnn_mem_handle);
  }

  return Status::OK();
}

Status QnnModel::ExecuteGraph(const Ort::KernelContext& context,
                              const logging::Logger& logger) {
  LOGS(logger, VERBOSE) << "QnnModel::ExecuteGraphs";
  const size_t num_inputs = context.GetInputCount();
  const size_t num_outputs = context.GetOutputCount();
  ORT_RETURN_IF_NOT(qnn_input_infos_.size() <= num_inputs, "Inconsistent input sizes");
  ORT_RETURN_IF_NOT(qnn_output_infos_.size() == num_outputs, "Inconsistent output sizes");

  using namespace qnn::utils;
  auto TensorDataSize = [](auto ort_tensor) -> size_t {
    auto tensor_type_and_shape = ort_tensor.GetTensorTypeAndShapeInfo();
    size_t length = tensor_type_and_shape.GetElementCount();
    ONNXTensorElementDataType element_type = tensor_type_and_shape.GetElementType();
    size_t element_size = GetElementSizeByType(element_type);
    return element_size * length;
  };

  std::vector<Qnn_Tensor_t> qnn_inputs;
  qnn_inputs.reserve(qnn_input_infos_.size());

  for (const auto& qnn_input_info : qnn_input_infos_) {
    LOGS(logger, VERBOSE) << "model_input = " << qnn_input_info.tensor_wrapper->GetName()
                          << " index = " << qnn_input_info.ort_index;
    auto ort_input_tensor = context.GetInput(qnn_input_info.ort_index);
    auto ort_tensor_size = TensorDataSize(ort_input_tensor);
    LOGS(logger, VERBOSE) << "Qnn tensor size: " << qnn_input_info.tensor_byte_size
                          << " Ort tensor size: " << ort_tensor_size;
    ORT_RETURN_IF_NOT(qnn_input_info.tensor_byte_size == ort_tensor_size,
                      "ORT Tensor data size does not match QNN tensor data size.");

    qnn_inputs.push_back(qnn_input_info.tensor_wrapper->GetQnnTensor());

    ORT_RETURN_IF_ERROR(BindQnnTensorMemoryToOrtValueMemory(
        logger,
        *qnn_backend_manager_,
        *static_cast<const OrtMemoryInfo*>(ort_input_tensor.GetTensorMemoryInfo()),
        const_cast<void*>(ort_input_tensor.GetTensorRawData()), qnn_input_info.tensor_byte_size,
        graph_info_->GraphContext(),
        qnn_inputs.back()));
  }

  std::vector<Qnn_Tensor_t> qnn_outputs;
  qnn_outputs.reserve(qnn_output_infos_.size());

  for (auto& qnn_output_info : qnn_output_infos_) {
    const std::string& model_output_name = qnn_output_info.tensor_wrapper->GetName();
    LOGS(logger, VERBOSE) << "model_output = " << model_output_name << " index = " << qnn_output_info.ort_index;
    const auto& ort_output_info = GetOutputInfo(model_output_name);
    const std::vector<int64_t>& output_shape = ort_output_info->shape_;
    auto ort_output_tensor = context.GetOutput(qnn_output_info.ort_index, output_shape.data(), output_shape.size());
    auto ort_tensor_size = TensorDataSize(ort_output_tensor);
    LOGS(logger, VERBOSE) << "Qnn tensor size: " << qnn_output_info.tensor_byte_size
                          << " Ort tensor size: " << ort_tensor_size;
    ORT_RETURN_IF_NOT(qnn_output_info.tensor_byte_size == ort_tensor_size,
                      "ORT Tensor data size does not match QNN tensor data size");

    qnn_outputs.push_back(qnn_output_info.tensor_wrapper->GetQnnTensor());

    ORT_RETURN_IF_ERROR(BindQnnTensorMemoryToOrtValueMemory(
        logger,
        *qnn_backend_manager_,
        *static_cast<const OrtMemoryInfo*>(ort_output_tensor.GetTensorMemoryInfo()),
        ort_output_tensor.GetTensorMutableRawData(), qnn_output_info.tensor_byte_size,
        graph_info_->GraphContext(),
        qnn_outputs.back()));
  }

  Qnn_ErrorHandle_t execute_status = QNN_GRAPH_NO_ERROR;
  {
    const auto& qnn_interface = qnn_backend_manager_->GetQnnInterface();

    // Acquire mutex before calling QNN APIs to support calling session.Run() from multiple threads.
    std::lock_guard<std::mutex> lock(graph_exec_mutex_);

    LOGS(logger, VERBOSE) << "Start execute QNN graph:" << graph_info_->Name();
    auto profile_backend_handle = qnn_backend_manager_->GetQnnProfileHandle();
    execute_status = qnn_interface.graphExecute(graph_info_->Graph(),
                                                qnn_inputs.data(),
                                                static_cast<uint32_t>(qnn_inputs.size()),
                                                qnn_outputs.data(),
                                                static_cast<uint32_t>(qnn_outputs.size()),
                                                profile_backend_handle,
                                                nullptr);

    // NOTE: This function returns immediately when profiling is disabled.
    // Extracting profiling data can be expensive, but it is typically only enabled for debugging purposes
    // and not in production. We can improve synchronization for event profiling if it becomes an issue.
    ORT_RETURN_IF_ERROR(qnn_backend_manager_->ExtractBackendProfilingInfo());
  }

  if (QNN_COMMON_ERROR_SYSTEM_COMMUNICATION == execute_status) {
    auto error_message = "NPU crashed. SSR detected. Caused QNN graph execute error. Error code: ";
    LOGS(logger, ERROR) << error_message << execute_status;
    return ORT_MAKE_STATUS(ONNXRUNTIME, ENGINE_ERROR, error_message, execute_status);
  }

  if (QNN_GRAPH_NO_ERROR != execute_status) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "QNN graph execute error. Error code: ", execute_status);
  }

  return Status::OK();
}

// Setup information for Qnn inputs/outputs used during execution.
Status QnnModel::SetupTensors(std::vector<QnnTensorInfo>& qnn_tensor_infos,
                              const std::vector<QnnTensorWrapper>& tensor_wrappers,
                              bool is_input) {
  size_t tensor_count = tensor_wrappers.size();
  ORT_RETURN_IF(0 == tensor_count, "Zero tensor size!");
  if (is_input) {
    // Resize qnn_tensor_infos according to the number of graph inputs.
    auto input_count = GetGraphInputCount();
    ORT_RETURN_IF(input_count < tensor_count,
                  "The count of graph inputs should be at least the count of tensor_wrapper!");
    qnn_tensor_infos.resize(input_count);
  } else {
    qnn_tensor_infos.resize(tensor_count);
  }

  for (auto& tensor_wrapper : tensor_wrappers) {
    ORT_RETURN_IF(utils::QnnTensorHasDynamicShape(tensor_wrapper.GetQnnTensor()),
                  "QNN tensor (", tensor_wrapper.GetName(), ") has dynamic shape. This is not supported yet.");

    const size_t length = utils::GetQnnTensorDataSizeInBytes(tensor_wrapper.GetTensorDims(),
                                                             tensor_wrapper.GetTensorDataType());
    const auto& tensor_name = tensor_wrapper.GetName();
    auto qnn_index = is_input ? GetGraphInputIndex(tensor_name) : GetOutputIndex(tensor_name);
    auto ort_index = is_input ? GetOrtInputIndex(tensor_name) : qnn_index;

    QnnTensorInfo& qnn_tensor_info = qnn_tensor_infos[qnn_index];
    qnn_tensor_info.tensor_wrapper = &tensor_wrapper;
    qnn_tensor_info.tensor_byte_size = static_cast<uint32_t>(length);
    qnn_tensor_info.ort_index = ort_index;
  }
  // The number of graph inputs and the number of tensor wrappers may not match.
  // - For example, for ResizeNearestNeighbor op, Qnn only cares about the 1st input,
  //   so the rest of the inputs are not converted to tensor wrappers.
  // - However, these remaining inputs still appear in the graph inputs, resulting in
  //   a discrepancy in the input quantities.
  // If not all inputs are used, erase the empty allocations in qnn_tensor_infos.
  if (is_input) {
    qnn_tensor_infos.erase(std::remove_if(qnn_tensor_infos.begin(),
                                          qnn_tensor_infos.end(),
                                          [](QnnTensorInfo qnn_tensor_info) { return qnn_tensor_info.tensor_wrapper == nullptr; }),
                           qnn_tensor_infos.end());
  }
  return Status::OK();
}

Status QnnModel::DeserializeGraphInfoFromBinaryInfo(const QnnSystemContext_GraphInfo_t& qnn_sys_ctx_graph_info,
                                                    const Qnn_ContextHandle_t& context) {
  std::vector<QnnTensorWrapper> input_tensor_wrappers;
  std::vector<QnnTensorWrapper> output_tensor_wrappers;

  std::string graph_name;
  Qnn_Tensor_t* input_tensors = nullptr;
  Qnn_Tensor_t* output_tensors = nullptr;
  uint32_t graph_input_num = 0;
  uint32_t graph_output_num = 0;
  if (qnn_sys_ctx_graph_info.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) {
    graph_name.assign(qnn_sys_ctx_graph_info.graphInfoV1.graphName);
    graph_input_num = qnn_sys_ctx_graph_info.graphInfoV1.numGraphInputs;
    graph_output_num = qnn_sys_ctx_graph_info.graphInfoV1.numGraphOutputs;

    input_tensors = qnn_sys_ctx_graph_info.graphInfoV1.graphInputs;
    output_tensors = qnn_sys_ctx_graph_info.graphInfoV1.graphOutputs;
  }
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 18)  // start from 2.25
  else if (qnn_sys_ctx_graph_info.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) {
    graph_name.assign(qnn_sys_ctx_graph_info.graphInfoV2.graphName);
    graph_input_num = qnn_sys_ctx_graph_info.graphInfoV2.numGraphInputs;
    graph_output_num = qnn_sys_ctx_graph_info.graphInfoV2.numGraphOutputs;

    input_tensors = qnn_sys_ctx_graph_info.graphInfoV2.graphInputs;
    output_tensors = qnn_sys_ctx_graph_info.graphInfoV2.graphOutputs;
  }
#endif
#if QNN_API_VERSION_MAJOR == 2 && (QNN_API_VERSION_MINOR >= 21)  // start from 2.28
  else if (qnn_sys_ctx_graph_info.version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3) {
    graph_name.assign(qnn_sys_ctx_graph_info.graphInfoV3.graphName);
    graph_input_num = qnn_sys_ctx_graph_info.graphInfoV3.numGraphInputs;
    graph_output_num = qnn_sys_ctx_graph_info.graphInfoV3.numGraphOutputs;

    input_tensors = qnn_sys_ctx_graph_info.graphInfoV3.graphInputs;
    output_tensors = qnn_sys_ctx_graph_info.graphInfoV3.graphOutputs;
  }
#endif
  else {
    return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Unsupported context graph info version.");
  }
  ORT_RETURN_IF(nullptr == input_tensors, "Graph from cached context doesn't have any inputs.");
  ORT_RETURN_IF(nullptr == output_tensors, "Graph from cached context doesn't have any outputs.");

  // Copy graph input
  for (size_t i = 0; i < graph_input_num; ++i) {
    QnnTensorWrapper tensorwrapper;
    ORT_RETURN_IF_ERROR(tensorwrapper.Init(input_tensors[i]));
    input_tensor_wrappers.push_back(std::move(tensorwrapper));
  }
  // Copy graph output
  for (size_t i = 0; i < graph_output_num; ++i) {
    QnnTensorWrapper tensorwrapper;
    ORT_RETURN_IF_ERROR(tensorwrapper.Init(output_tensors[i]));
    output_tensor_wrappers.push_back(std::move(tensorwrapper));
  }

  Qnn_GraphHandle_t graph;
  auto qnn_interface = qnn_backend_manager_->GetQnnInterface();
  auto rt = qnn_interface.graphRetrieve(context, graph_name.c_str(), &graph);
  ORT_RETURN_IF(QNN_SUCCESS != rt, "Failed to retrieve QNN graph.");

  graph_info_ = std::make_unique<GraphInfo>(graph,
                                            graph_name,
                                            context,
                                            std::move(input_tensor_wrappers),
                                            std::move(output_tensor_wrappers));

  return Status::OK();
}

}  // namespace qnn
}  // namespace onnxruntime
