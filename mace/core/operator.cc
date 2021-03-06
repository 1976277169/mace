// Copyright 2018 Xiaomi, Inc.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sstream>
#include <map>
#include <memory>
#include <vector>

#include "mace/core/operator.h"

namespace mace {

OpConstructContext::OpConstructContext(Workspace *ws)
    : operator_def_(nullptr), ws_(ws), device_(nullptr) {}
OpConstructContext::OpConstructContext(OperatorDef *operator_def,
                                       Workspace *ws,
                                       Device *device)
    : operator_def_(operator_def), ws_(ws), device_(device) {}

OpInitContext::OpInitContext(Workspace *ws, Device *device)
    : ws_(ws), device_(device) {}

Operation::Operation(OpConstructContext *context)
    : operator_def_(std::make_shared<OperatorDef>(*(context->operator_def())))
{}

MaceStatus Operation::Init(OpInitContext *context) {
  Workspace *ws = context->workspace();
  for (const std::string &input_str : operator_def_->input()) {
    const Tensor *tensor = ws->GetTensor(input_str);
    MACE_CHECK(tensor != nullptr, "op ", operator_def_->type(),
               ": Encountered a non-existing input tensor: ", input_str);
    inputs_.push_back(tensor);
  }
  // TODO(liuqi): filter transform
  for (int i = 0; i < operator_def_->output_size(); ++i) {
    const std::string output_str = operator_def_->output(i);
    if (ws->HasTensor(output_str)) {
      // TODO(liuqi): Workspace should pre-allocate all of the output tensors
      outputs_.push_back(ws->GetTensor(output_str));
    } else {
      MACE_CHECK(
          operator_def_->output_type_size() == 0 ||
              operator_def_->output_size() == operator_def_->output_type_size(),
          "operator output size != operator output type size",
          operator_def_->output_size(),
          operator_def_->output_type_size());
      DataType output_type;
      if (i < operator_def_->output_type_size()) {
        output_type = operator_def_->output_type(i);
      } else {
        output_type = static_cast<DataType>(
            ProtoArgHelper::GetOptionalArg<OperatorDef, int>(
            *operator_def_, "T", static_cast<int>(DT_FLOAT)));
      }
      outputs_.push_back(MACE_CHECK_NOTNULL(ws->CreateTensor(
          output_str, context->device()->allocator(), output_type)));

      if (i < operator_def_->output_shape_size()) {
        std::vector<index_t>
            shape_configured(operator_def_->output_shape(i).dims_size());
        for (size_t dim = 0; dim < shape_configured.size(); ++dim) {
          shape_configured[dim] = operator_def_->output_shape(i).dims(dim);
        }
        ws->GetTensor(output_str)->SetShapeConfigured(shape_configured);
      }
    }
  }
  return MaceStatus::MACE_SUCCESS;
}

// op registry
namespace {
class OpKeyBuilder {
 public:
  explicit OpKeyBuilder(const std::string &op_name);

  OpKeyBuilder &Device(DeviceType device);

  OpKeyBuilder &TypeConstraint(const char *attr_name,
                               DataType allowed);

  const std::string Build();

 private:
  std::string op_name_;
  DeviceType device_type_;
  std::map<std::string, DataType> type_constraint_;
};

OpKeyBuilder::OpKeyBuilder(const std::string &op_name) : op_name_(op_name) {}

OpKeyBuilder &OpKeyBuilder::Device(DeviceType device) {
  device_type_ = device;
  return *this;
}

OpKeyBuilder &OpKeyBuilder::TypeConstraint(const char *attr_name,
                                           DataType allowed) {
  type_constraint_[attr_name] = allowed;
  return *this;
}

const std::string OpKeyBuilder::Build() {
  static const std::vector<std::string> type_order = {"T"};
  std::stringstream ss;
  ss << op_name_;
  ss << device_type_;
  for (auto type : type_order) {
    ss << type << "_" << DataTypeToString(type_constraint_[type]);
  }

  return ss.str();
}
}  // namespace

void OpRegistrationInfo::AddDevice(mace::DeviceType device) {
  devices.insert(device);
}

void OpRegistrationInfo::Register(const std::string &key, OpCreator creator) {
  VLOG(3) << "Registering: " << key;
  MACE_CHECK(creators.count(key) == 0, "Key already registered: ", key);
  creators[key] = creator;
}

MaceStatus OpRegistryBase::Register(const std::string &op_type,
                                const mace::DeviceType device_type,
                                const mace::DataType dt,
                                mace::OpRegistrationInfo::OpCreator creator) {
  if (registry_.count(op_type) == 0) {
    registry_[op_type] = std::unique_ptr<OpRegistrationInfo>(
        new OpRegistrationInfo);
  }
  registry_[op_type]->AddDevice(device_type);

  std::string op_key = OpKeyBuilder(op_type)
      .Device(device_type)
      .TypeConstraint("T", dt)
      .Build();
  registry_.at(op_type)->Register(op_key, creator);
  return MaceStatus::MACE_SUCCESS;
}

const std::set<DeviceType> OpRegistryBase::AvailableDevices(
    const std::string &op_type) const {
  MACE_CHECK(registry_.count(op_type) != 0,
             op_type, " operation is not registered.");

  return registry_.at(op_type)->devices;
}


std::unique_ptr<Operation> OpRegistryBase::CreateOperation(
    OpConstructContext *context,
    DeviceType device_type,
    const NetMode mode) const {
  OperatorDef *operator_def = context->operator_def();
  const DataType dtype = static_cast<DataType>(
      ProtoArgHelper::GetOptionalArg<OperatorDef, int>(
          *operator_def, "T", static_cast<int>(DT_FLOAT)));
  const int op_mode_i = ProtoArgHelper::GetOptionalArg<OperatorDef, int>(
      *operator_def, "mode", static_cast<int>(NetMode::NORMAL));
  const NetMode op_mode = static_cast<NetMode>(op_mode_i);
  VLOG(3) << "Creating operator " << operator_def->name() << "("
          << operator_def->type() << "<" << dtype << ">" << ") on "
          << device_type;
  if (op_mode == mode) {
    const std::string op_type = context->operator_def()->type();
    MACE_CHECK(registry_.count(op_type) != 0,
               op_type, " operation is not registered.");

    std::string key = OpKeyBuilder(op_type)
        .Device(device_type)
        .TypeConstraint("T", dtype)
        .Build();
    if (registry_.at(op_type)->creators.count(key) == 0) {
      LOG(FATAL) << "Key not registered: " << key;
    }
    return registry_.at(op_type)->creators.at(key)(context);
  } else {
    return nullptr;
  }
}
}  // namespace mace
