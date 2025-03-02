/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/jit/xla_launch_util.h"

#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/cleanup/cleanup.h"
#include "tensorflow/compiler/jit/variable_info.h"
#include "tensorflow/compiler/jit/variable_info_util.h"
#include "tensorflow/compiler/tf2xla/const_analysis.h"
#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/xla_compiler.h"
#include "tensorflow/compiler/xla/client/local_client.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/common_runtime/gpu_device_context.h"
#include "tensorflow/core/framework/allocator.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/refcount.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/tfrt/common/async_value_tensor.h"
#include "tensorflow/core/util/stream_executor_util.h"
#include "tensorflow/tsl/platform/status.h"

namespace tensorflow {
namespace {
using xla::ScopedShapedBuffer;
using xla::ShapedBuffer;

// Fetch the platform Id from device.
se::Platform::Id XlaPlatformInfoFromDevice(DeviceBase* device_base) {
  auto device = static_cast<Device*>(device_base);
  se::Platform::Id platform_id = nullptr;
  if (device->device_type() == DEVICE_CPU) {
    platform_id = se::host::kHostPlatformId;
  }

  return platform_id;
}

absl::flat_hash_map<int, int> CreateVariableLookup(
    const std::vector<VariableInfo>& variables) {
  absl::flat_hash_map<int, int> variable_lookup;
  for (int i = 0; i < variables.size(); i++) {
    variable_lookup[variables[i].index()] = i;
  }
  return variable_lookup;
}

}  // anonymous namespace

std::vector<const Tensor*> InputsFromContext(OpKernelContext* ctx) {
  std::vector<const Tensor*> inputs;
  inputs.reserve(ctx->num_inputs());
  for (int input_idx = 0; input_idx < ctx->num_inputs(); input_idx++) {
    inputs.push_back(&ctx->input(input_idx));
  }
  return inputs;
}

StatusOr<std::vector<int>> GetConstantInputIndicesFromContext(
    OpKernelContext* ctx) {
  std::vector<int> constant_input_indices;
  TF_RETURN_IF_ERROR(GetCompileTimeConstInputs(
      &ctx->op_kernel(), &constant_input_indices, ctx->function_library()));
  if (!absl::c_all_of(constant_input_indices, [&](int idx) {
        return ctx->input_memory_type(idx) == HOST_MEMORY;
      })) {
    return errors::Internal("Unexpected device placement for a constant input");
  }
  return constant_input_indices;
}

XlaComputationLaunchContext::XlaComputationLaunchContext(
    xla::LocalClient* client, se::DeviceMemoryAllocator* xla_allocator,
    int device_ordinal, bool allocate_xla_tensors, bool use_multiple_streams)
    : client_(client),
      xla_allocator_(xla_allocator),
      allocate_xla_tensors_(allocate_xla_tensors),
      use_multiple_streams_(use_multiple_streams),
      device_ordinal_(device_ordinal) {
  if (use_multiple_streams_) {
    CHECK(allocate_xla_tensors_) << "To use multiple streams correctly we must "
                                    "be allocating XLA tensors!";
  }
}

// Fills in `execution_input` with `buffer` for `index`.
static void PopulateExecutionInputBuffer(xla::ExecutionInput& execution_input,
                                         xla::ShapeIndex index,
                                         se::DeviceMemoryBase buffer,
                                         bool donate_buffer, int device_ordinal,
                                         se::DeviceMemoryAllocator* allocator) {
  xla::MaybeOwningDeviceMemory* in_buffer =
      execution_input.MutableBuffer(index);
  if (donate_buffer) {
    // Here we pass ownership of the buffer to execution_input without releasing
    // ownership from the caller of PopulateExecutionInputBuffer. If execution
    // succeeds, we'll take back that duplicate ownership in
    // GetOrCreateTensorForOutput. If execution fails, the ExecutionInput will
    // release that duplicate ownership automatically.
    *in_buffer = se::OwningDeviceMemory(buffer, device_ordinal, allocator);
  } else {
    *in_buffer = buffer;
  }
}

StatusOr<std::vector<xla::ExecutionInput>>
XlaComputationLaunchContext::PopulateInputs(
    OpKernelContext* ctx,
    const XlaCompiler::CompilationResult* compilation_result,
    const std::map<int, const Tensor*>& resource_vars,
    int missing_ctx_input_prefix,
    const xla::HloInputOutputAliasConfig& input_output_alias) {
  std::vector<xla::ExecutionInput> arguments;
  arguments.reserve(compilation_result->xla_input_shapes.size());

  for (int i = 0, end = compilation_result->xla_input_shapes.size(); i < end;
       ++i) {
    int arg_num = compilation_result->input_mapping[i];
    CHECK_GE(arg_num, missing_ctx_input_prefix);
    const xla::Shape& device_shape = compilation_result->xla_input_shapes[i];
    const xla::Shape& host_shape =
        xla::ShapeUtil::DeviceShapeToHostShape(device_shape);

    bool is_resource_variable = resource_vars.count(arg_num);
    bool is_updated_resource_variable =
        is_resource_variable &&
        absl::c_any_of(compilation_result->resource_updates,
                       [&](const XlaCompiler::ResourceUpdate& update) {
                         // XlaCompiler records `arg_num` (instead of kernel
                         // parameters) in `resource_updates`.
                         return update.input_index == arg_num &&
                                update.modified;
                       });

    const Tensor* t = is_resource_variable
                          ? resource_vars.at(arg_num)
                          : &(ctx->input(arg_num - missing_ctx_input_prefix));
    CHECK(t);
    bool donate_buffer =
        t->RefCountIsOne() && is_updated_resource_variable &&
        input_output_alias.ParameterHasAlias(i, xla::ShapeIndex{});
    VLOG(3) << "Processing input: " << i
            << "; is_resource_variable=" << is_resource_variable
            << "; is_updated_resource_variable=" << is_updated_resource_variable
            << "; donate_buffer=" << donate_buffer;

    if (use_multiple_streams_) {
      CHECK(ctx->op_device_context() && ctx->op_device_context()->stream())
          << "Must have a stream available when using XLA tensors!";
      XlaTensor* xla_tensor = XlaTensor::FromTensor(t);
      CHECK(xla_tensor);
      xla_tensor->WaitForDefinitionEventOnStream(
          ctx->op_device_context()->stream());
    }

    arguments.emplace_back(device_shape, host_shape);
    xla::ExecutionInput& execution_input = arguments.back();
    se::DeviceMemoryBase dmem = XlaTensor::DeviceMemoryFromTensor(*t);
    PopulateExecutionInputBuffer(execution_input, xla::ShapeIndex{}, dmem,
                                 donate_buffer, device_ordinal_,
                                 xla_allocator_);
  }
  return std::move(arguments);
}

// Construct the tensor for the given type and buffer.
static Tensor MakeTensor(DataType dtype, const TensorShape& shape,
                         se::DeviceMemoryBase buffer, Allocator* allocator) {
  size_t expected_size = shape.num_elements() * DataTypeSize(dtype);
  auto* tensor_buffer = new XlaTensorBuffer(buffer.opaque(), expected_size,
                                            buffer.size(), allocator);
  Tensor t(dtype, shape, tensor_buffer);
  tensor_buffer->Unref();
  return t;
}

// Get aliased tensor from output, or make a new one for the corresponding
// output operation. Transfers ownership of the buffer from output to the
// returned tensor.
static StatusOr<Tensor> GetOrCreateTensorForOutput(
    xla::ScopedShapedBuffer& output, int output_num, OpKernelContext* ctx,
    int missing_ctx_input_prefix,
    const xla::HloInputOutputAliasConfig& input_output_alias,
    absl::Span<const int> input_mapping,
    const std::map<int, const Tensor*>& resource_vars_snapshots,
    DataType output_dtype, const TensorShape& output_shape,
    Allocator* output_allocator, bool allocate_xla_tensors, se::Stream* stream,
    bool use_multiple_streams, std::shared_ptr<se::Event> definition_event) {
  xla::ShapeIndex output_index = input_output_alias.shape().IsTuple()
                                     ? xla::ShapeIndex({output_num})
                                     : xla::ShapeIndex({});
  CHECK(input_output_alias.shape().IsTuple() || output_num == 0);
  if (std::optional<xla::HloInputOutputAliasConfig::Alias> alias =
          input_output_alias.GetAliasedParameter(output_index)) {
    VLOG(3) << "Found alias: " << alias->ToString();
    int tf_param =
        input_mapping[alias->parameter_number] - missing_ctx_input_prefix;
    const Tensor input_tensor =
        ctx->input(tf_param).dtype() != DT_RESOURCE
            ? ctx->input(tf_param)
            : *resource_vars_snapshots.at(missing_ctx_input_prefix + tf_param);
    se::DeviceMemoryBase input_buffer =
        XlaTensor::DeviceMemoryFromTensor(input_tensor);
    se::DeviceMemoryBase output_buffer = output.buffer({output_num});
    if (input_buffer.opaque() == output_buffer.opaque()) {
      // In the case of a donated buffer, both input_tensor and output think
      // they have ownership of the buffer (see comment in
      // PopulateExecutionInputBuffer). Release ownership from output to avoid
      // double free.
      output.set_buffer(se::OwningDeviceMemory(), {output_num});
      return input_tensor;
    }
  }

  if (allocate_xla_tensors) {
    Tensor output_tensor;
    TF_RETURN_IF_ERROR(
        ctx->allocate_temp(output_dtype, output_shape, &output_tensor));
    if (output_tensor.TotalBytes() > 0) {
      XlaTensor* xla_tensor = XlaTensor::FromTensor(&output_tensor);
      TF_RET_CHECK(xla_tensor);
      xla_tensor->set_shaped_buffer(output.TakeSubTree({output_num}));
      if (use_multiple_streams) {
        xla_tensor->ResetDefinitionEvent(definition_event, stream);
      }
    }
    return output_tensor;
  }

  se::DeviceMemoryBase output_buffer = output.buffer({output_num});
  Tensor output_tensor =
      MakeTensor(output_dtype, output_shape, output_buffer, output_allocator);
  output.set_buffer(se::OwningDeviceMemory(), {output_num});
  return output_tensor;
}

// Sets output `output_num` for `ctx` provided it is known at a compile time.
Status SetOutputForConstant(
    OpKernelContext* ctx, bool requires_copy_to_device,
    const XlaCompiler::CompilationResult* compilation_result, int output_num) {
  CHECK(compilation_result->outputs[output_num].is_constant);
  const Tensor& const_tensor =
      compilation_result->outputs[output_num].constant_value;
  Tensor* output_tensor;
  if (requires_copy_to_device && const_tensor.TotalBytes() > 0) {
    // Copy host -> device. (Empty tensors don't have backing buffers.)
    // Manually allocate memory so we can allocate as much memory as the device
    // requires (as given by GetByteSizeRequirement). This avoids
    // XlaTransferManager having to reallocate the device buffer later if
    // XlaTransferManager is used.
    VLOG(1) << "Constant output tensor on device";

    TF_RETURN_IF_ERROR(
        ctx->allocate_output(output_num, const_tensor.shape(), &output_tensor));
    Device* device = dynamic_cast<Device*>(ctx->device());
    if (device == nullptr) {
      return errors::Internal("DeviceBase was not a Device.");
    }
    ctx->op_device_context()->CopyCPUTensorToDevice(
        &const_tensor, device, output_tensor,
        [&](Status status) { TF_CHECK_OK(status); });

    if (device->device_type() == DEVICE_GPU) {
      // The GPUDeviceContext enqueues the host->device transfer in a
      // separate stream from the main compute stream. We must ensure the
      // compute stream is synchronized with the host->device transfer
      // stream now otherwise we will create a race condition.
      auto* gpu_device_context =
          static_cast<GPUDeviceContext*>(ctx->op_device_context());
      gpu_device_context->stream()->ThenWaitFor(
          gpu_device_context->host_to_device_stream());
    }
  } else {
    // No copy required.
    ctx->set_output(output_num, const_tensor);
    output_tensor = ctx->mutable_output(output_num);
  }
  return OkStatus();
}

static StatusOr<Var*> GetOrCreateResourceVar(
    OpKernelContext* ctx, const ResourceHandle& handle,
    const XlaCompiler::ResourceUpdate& write) {
  Var* variable = nullptr;
  TF_RETURN_IF_ERROR(
      LookupOrCreateResource<Var>(ctx, handle, &variable, [&write](Var** ptr) {
        *ptr = new Var(write.type);
        return OkStatus();
      }));
  return variable;
}

StatusOr<std::vector<VariableInfo>> GatherVariableInfo(
    OpKernelContext* ctx,
    const XlaCompiler::CompilationResult& compilation_result,
    int missing_ctx_input_prefix) {
  std::vector<VariableInfo> out;
  out.reserve(compilation_result.resource_updates.size());
  for (int i = 0; i < compilation_result.resource_updates.size(); ++i) {
    const XlaCompiler::ResourceUpdate& write =
        compilation_result.resource_updates[i];
    int actual_input_index = write.input_index - missing_ctx_input_prefix;
    if (actual_input_index < 0 || actual_input_index >= ctx->num_inputs()) {
      return errors::Internal("Invalid input index for variable write.");
    }

    const ResourceHandle handle = HandleFromInput(ctx, actual_input_index);
    TF_ASSIGN_OR_RETURN(Var * variable,
                        GetOrCreateResourceVar(ctx, handle, write));
    out.emplace_back(actual_input_index, handle.name(), variable,
                     handle.definition_stack_trace());
  }
  return std::move(out);
}

Status XlaComputationLaunchContext::PopulateOutputs(
    OpKernelContext* ctx,
    const XlaCompiler::CompilationResult* compilation_result,
    ScopedShapedBuffer output, int missing_ctx_input_prefix,
    absl::Span<VariableInfo> variable_infos,
    const xla::HloInputOutputAliasConfig& input_output_alias,
    const std::map<int, const Tensor*>& resource_vars) {
  se::Stream* stream =
      ctx->op_device_context() ? ctx->op_device_context()->stream() : nullptr;
  Allocator* allocator = ctx->device()->GetAllocator({});

  // Computation output should always be a tuple.
  VLOG(2) << "Result tuple shape: " << output.on_host_shape().DebugString();
  VLOG(2) << "Result tuple shape (on device): "
          << output.on_device_shape().DebugString();
  CHECK_EQ(ctx->num_outputs(), compilation_result->outputs.size());

  // If the on-host-shape isn't a tuple, create a new single-element tuple
  // buffer with a nullptr root index table. This allows the code below to treat
  // output as a tuple unconditionally.
  if (!output.on_host_shape().IsTuple()) {
    ShapedBuffer nontuple_buffer = output.release();
    ShapedBuffer buffer(
        xla::ShapeUtil::MakeTupleShape({nontuple_buffer.on_host_shape()}),
        xla::ShapeUtil::MakeTupleShape({nontuple_buffer.on_device_shape()}),
        output.device_ordinal());
    buffer.buffers().CopySubtreeFrom(nontuple_buffer.buffers(),
                                     /*source_base_index=*/{},
                                     /*target_base_index=*/{0});
    output = ScopedShapedBuffer(std::move(buffer), output.memory_allocator());
  }

  std::shared_ptr<se::Event> definition_event;
  if (use_multiple_streams_ && stream) {
    definition_event = std::make_shared<se::Event>(stream->parent());
    if (!definition_event->Init()) {
      return errors::Internal("Failed to initialize tensor definition event.");
    }
    stream->ThenRecordEvent(definition_event.get());
  }

  for (const XlaOutputDescription& descr : compilation_result->outputs) {
    if (descr.type == DT_VARIANT) {
      return errors::Unimplemented(
          "Support for TensorList crossing the XLA/TF boundary "
          "is not implemented");
    }
  }

  std::vector<TensorShape> output_tensor_shapes;
  output_tensor_shapes.reserve(ctx->num_outputs());
  if (output.on_host_shape().is_dynamic()) {
    const se::Platform* platform = nullptr;
    if (stream != nullptr) {
      platform = stream->parent()->platform();
    } else {
      // Stream is not set for the host platform.
      TF_ASSIGN_OR_RETURN(platform,
                          se::MultiPlatformManager::PlatformWithId(
                              XlaPlatformInfoFromDevice(ctx->device())));
    }
    TF_ASSIGN_OR_RETURN(auto transfer_manager,
                        xla::TransferManager::GetForPlatform(platform));

    xla::Shape output_device_shape = output.on_device_shape();
    TF_RETURN_IF_ERROR(transfer_manager->ReadDynamicShapes(
        stream, &output, &output_device_shape));

    output.set_shapes(output_device_shape, output_device_shape);
    for (int i = 0; i < ctx->num_outputs(); ++i) {
      const xla::Shape& subshape =
          xla::ShapeUtil::GetSubshape(output_device_shape, {i});
      TensorShape shape;
      TF_RETURN_IF_ERROR(XLAShapeToTensorShape(subshape, &shape));
      output_tensor_shapes.push_back(shape);
    }
  } else {
    for (int i = 0; i < ctx->num_outputs(); ++i) {
      output_tensor_shapes.push_back(compilation_result->outputs[i].shape);
    }
  }

  // Copy XLA results to the OpOutputList.
  int output_num = 0;
  for (int i = 0, end = ctx->num_outputs(); i < end; ++i) {
    const TensorShape& shape = output_tensor_shapes[i];
    const DataType& type = compilation_result->outputs[i].type;
    VLOG(2) << "Populating output for retval " << i << " shape "
            << shape.DebugString() << " type " << DataTypeString(type);

    if (compilation_result->outputs[i].is_constant) {
      TF_RETURN_IF_ERROR(SetOutputForConstant(
          ctx, /*requires_copy_to_device=*/stream != nullptr,
          compilation_result, i));
    } else if (type == DT_RESOURCE) {
      int input_index =
          compilation_result->outputs[i].input_index - missing_ctx_input_prefix;
      TF_RET_CHECK(input_index >= 0 && input_index < ctx->num_inputs())
          << "Invalid input for outputs " << i << ": " << input_index;
      ctx->set_output(i, ctx->input(input_index));
    } else {
      TF_ASSIGN_OR_RETURN(
          Tensor output_tensor,
          GetOrCreateTensorForOutput(
              output, output_num, ctx, missing_ctx_input_prefix,
              input_output_alias, compilation_result->input_mapping,
              resource_vars, ctx->expected_output_dtype(i), shape, allocator,
              allocate_xla_tensors_, stream, use_multiple_streams_,
              definition_event));
      ctx->set_output(i, output_tensor);
      ++output_num;
    }
  }

  // input_index -> index into variable_infos.
  absl::flat_hash_map<int, int> variable_info_lookup;
  for (int i = 0; i < variable_infos.size(); i++) {
    variable_info_lookup.emplace(variable_infos[i].index(), i);
  }

  // Apply variable updates, if any.
  for (int i = 0, end = compilation_result->resource_updates.size(); i < end;
       ++i) {
    const XlaCompiler::ResourceUpdate& write =
        compilation_result->resource_updates[i];
    int actual_input_index = write.input_index - missing_ctx_input_prefix;
    CHECK_GE(actual_input_index, 0);
    CHECK_LT(actual_input_index, ctx->num_inputs());
    Var* var = variable_infos[variable_info_lookup[actual_input_index]].var();
    CHECK(var);

    VLOG(2) << "Updating variable #" << i
            << " at input index: " << actual_input_index << " with shape "
            << write.shape.DebugString() << "; variable tensor has shape: "
            << var->tensor()->shape().DebugString();

    if (var->is_initialized && var->tensor()->dtype() != write.type) {
      return errors::Internal("Mismatched type in variable write");
    }

    TF_ASSIGN_OR_RETURN(
        Tensor output_tensor,
        GetOrCreateTensorForOutput(output, output_num, ctx,
                                   missing_ctx_input_prefix, input_output_alias,
                                   compilation_result->input_mapping,
                                   resource_vars, write.type, write.shape,
                                   allocator, allocate_xla_tensors_, stream,
                                   use_multiple_streams_, definition_event));
    var->is_initialized |= write.modified;
    *var->tensor() = output_tensor;
    ++output_num;
  }
  return OkStatus();
}

StatusOr<std::vector<XlaCompiler::Argument>>
XlaComputationLaunchContext::BuildXlaCompilerArguments(
    absl::Span<int const> must_be_constant_idxs,
    absl::Span<const Tensor* const> inputs,
    absl::Span<VariableInfo const> variable_args, Device* device) {
  CHECK(absl::c_is_sorted(must_be_constant_idxs));
  VLOG(2) << "Must be const args: {"
          << absl::StrJoin(must_be_constant_idxs, ",") << "} out of "
          << inputs.size() << " args";
  std::vector<XlaCompiler::Argument> out;
  out.resize(inputs.size());

  // TODO(cheshire): Avoid duplication with framework/op_kernel.h
  DeviceContext* device_context = nullptr;
  TF_RETURN_IF_ERROR(device->TryGetDeviceContext(&device_context));
  bool using_default_context = false;
  auto cleanup = absl::MakeCleanup([&] {
    if (device_context != nullptr && !using_default_context) {
      device_context->Unref();
    }
  });
  if (device_context == nullptr) {
    using_default_context = true;
    auto* dev_info = device->tensorflow_accelerator_device_info();
    if (dev_info) device_context = dev_info->default_context;
  }

  absl::flat_hash_map<int, const VariableInfo*> variable_info_lookup;
  TF_CHECK_OK(CreateVariableInfoLookup(variable_args, variable_info_lookup));
  for (int64_t input_num = 0; input_num < inputs.size(); ++input_num) {
    const Tensor* input = inputs[input_num];

    XlaCompiler::Argument& arg = out[input_num];
    if (variable_info_lookup.count(input_num)) {
      // Handles resource variables.
      TF_RET_CHECK(input->dtype() == DT_RESOURCE);
      const VariableInfo& variable = *variable_info_lookup[input_num];
      arg.name = std::string(variable.name());
      arg.kind = XlaCompiler::Argument::kResource;
      arg.resource_kind = XlaResource::kVariable;
      arg.definition_stack_trace = variable.definition_stack_trace();
      if (variable.var() && variable.var()->is_initialized) {
        const Tensor* value = variable.var()->tensor();
        arg.type = value->dtype();
        arg.shape = value->shape();
        arg.initialized = true;
      } else {
        // The values of uninitialized variables are not passed as inputs, since
        // they are meaningless. However, it is legal to assign to a resource
        // variable for the first time inside the XLA computation, so we do
        // permit uninitialized variables.
        arg.initialized = false;
        arg.type = DT_INVALID;
        arg.shape = TensorShape();
      }

      if (absl::c_binary_search(must_be_constant_idxs, input_num)) {
        TF_RET_CHECK(variable.var() && variable.var()->is_initialized);
        const Tensor* value = variable.var()->tensor();
        Tensor value_on_host(value->dtype(), value->shape());
        if (!device_context) {
          value_on_host = *value;
        } else {
          TF_RETURN_IF_ERROR(device_context->CopyDeviceTensorToCPUSync(
              value, "", device, &value_on_host));
        }
        arg.kind = XlaCompiler::Argument::kConstantResource;
        arg.constant_value = value_on_host;
      }
    } else if (absl::c_binary_search(must_be_constant_idxs, input_num)) {
      arg.kind = XlaCompiler::Argument::kConstant;
      arg.type = input->dtype();
      arg.shape = input->shape();
      arg.constant_value = *input;
    } else {
      // Normal inputs.
      TF_RET_CHECK(input->dtype() != DT_RESOURCE);
      if (input->NumElements() > 0) {
        arg.kind = XlaCompiler::Argument::kParameter;
      } else {
        arg.kind = XlaCompiler::Argument::kConstant;
        arg.constant_value = *input;
      }
      arg.type = input->dtype();
      arg.shape = input->shape();
    }
  }

  return out;
}

std::vector<xla::PjRtBuffer*> PreparePjRtExecutableArguments(
    const std::vector<int>& input_mapping,
    const std::vector<const Tensor*>& inputs,
    const std::vector<VariableInfo>& variables) {
  const auto& variable_lookup = CreateVariableLookup(variables);

  std::vector<xla::PjRtBuffer*> args;
  args.reserve(input_mapping.size());

  for (auto arg_num : input_mapping) {
    const Tensor* tensor;
    if (auto it = variable_lookup.find(arg_num); it != variable_lookup.end()) {
      tensor = variables[it->second].var()->tensor();
    } else {
      tensor = inputs[arg_num];
    }
    AsyncValueTensor* av_tensor = AsyncValueTensor::FromTensor(tensor);

    if (av_tensor->GetBuffer() == nullptr) {
      // TODO(b/260799971): verify size 0 argument is supported.
      CHECK_EQ(tensor->NumElements(), 0);  // Crash OK
      continue;
    }
    args.push_back(av_tensor->GetBuffer().get());
  }

  return args;
}

Status PopulateCtxOutputsFromPjRtExecutableOutputs(
    const std::vector<const Tensor*>& inputs,
    const std::vector<VariableInfo>& variables,
    const XlaCompiler::CompilationResult& compilation_result,
    std::vector<std::unique_ptr<xla::PjRtBuffer>>& executable_outputs,
    OpKernelContext* ctx) {
  const auto& variable_lookup = CreateVariableLookup(variables);

  // Copy XLA results to the OpOutputList.
  int output_num = 0;
  for (int i = 0, end = ctx->num_outputs(); i < end; ++i) {
    const DataType& type = compilation_result.outputs[i].type;
    VLOG(2) << "Populating output for retval " << i << " type "
            << DataTypeString(type);

    if (compilation_result.outputs[i].is_constant) {
      bool requires_copy_to_device = GetDeviceType(ctx) != DEVICE_CPU;
      TF_RETURN_IF_ERROR(SetOutputForConstant(ctx, requires_copy_to_device,
                                              &compilation_result, i));
    } else if (type == DT_RESOURCE) {
      int input_index = compilation_result.outputs[i].input_index;
      TF_RET_CHECK(input_index >= 0 && input_index < ctx->num_inputs())
          << "Invalid input for outputs " << i << ": " << input_index;
      ctx->set_output(i, *inputs[input_index]);
    } else {
      Tensor* output_tensor;
      TensorShape shape = TensorShape(
          executable_outputs[output_num]->on_device_shape().dimensions());
      TF_RETURN_IF_ERROR(ctx->allocate_output(i, shape, &output_tensor));
      auto output_avt = AsyncValueTensor::FromTensor(output_tensor);
      output_avt->SetBuffer(std::move(executable_outputs[output_num]));
      ++output_num;
    }
  }

  // Apply variable updates, if any.
  for (int i = 0, end = compilation_result.resource_updates.size(); i < end;
       ++i) {
    const XlaCompiler::ResourceUpdate& write =
        compilation_result.resource_updates[i];
    int actual_input_index = write.input_index;
    CHECK_GE(actual_input_index, 0);                  // Crash OK
    CHECK_LT(actual_input_index, ctx->num_inputs());  // Crash OK
    auto it = variable_lookup.find(actual_input_index);
    if (it == variable_lookup.end()) {
      continue;
    }
    Var* var = variables[it->second].var();
    CHECK(var);  // Crash OK

    VLOG(2) << "Updating variable #" << i
            << " at input index: " << actual_input_index << " with shape "
            << write.shape.DebugString() << "; variable tensor has shape: "
            << var->tensor()->shape().DebugString();

    if (var->is_initialized && var->tensor()->dtype() != write.type) {
      return errors::Internal("Mismatched type in variable write");
    }

    TF_RETURN_IF_ERROR(ctx->allocate_temp(
        var->tensor()->dtype(), var->tensor()->shape(), var->tensor()));
    AsyncValueTensor::FromTensor(var->tensor())
        ->SetBuffer(std::move(executable_outputs[output_num]));
    var->is_initialized |= write.modified;
    ++output_num;
  }
  return OkStatus();
}

xla::ExecuteOptions GetPjRtExecuteOptions() {
  xla::ExecuteOptions options;
  options.arguments_are_tupled = false;
  options.untuple_result = true;
  // Note: TF does not use PJRT host callbacks as of today. Setting this option
  // to true to workaround an ExecuteOptions check: [1].
  //
  // [1]:
  // tensorflow/compiler/xla/pjrt/pjrt_c_api_client.cc;l=923-927;rcl=519286815
  options.use_major_to_minor_data_layout_for_callbacks = true;
  return options;
}

DeviceType GetDeviceType(OpKernelContext* ctx) {
  auto* device =
      tensorflow::down_cast<Device*>(ctx->device()->UnderlyingDevice());
  return DeviceType(device->device_type());
}

int GetDeviceOrdinal(const DeviceBase* device) {
  return device->parsed_name().id;
}

Status RunPjRtExecutable(
    const xla::PjRtClient& pjrt_client,
    const std::vector<const Tensor*>& inputs,
    const std::vector<VariableInfo>& variables,
    const XlaCompiler::CompilationResult& compilation_result,
    xla::PjRtLoadedExecutable* executable, OpKernelContext* ctx) {
  TF_ASSIGN_OR_RETURN(
      xla::PjRtDevice * device,
      pjrt_client.LookupAddressableDevice(GetDeviceOrdinal(ctx->device())));

  const std::vector<xla::PjRtBuffer*> executable_args =
      PreparePjRtExecutableArguments(compilation_result.input_mapping, inputs,
                                     variables);
  // TODO(b/257548614): currently PJRT is compiled as portable (num_replica = 1
  // and num_partition = 1). Support multiple partitions case.
  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<xla::PjRtBuffer>> execute_outputs,
      executable->ExecutePortable(executable_args, device,
                                  GetPjRtExecuteOptions()));

  TF_RETURN_IF_ERROR(PopulateCtxOutputsFromPjRtExecutableOutputs(
      inputs, variables, compilation_result, execute_outputs, ctx));
  return OkStatus();
}

}  // namespace tensorflow
