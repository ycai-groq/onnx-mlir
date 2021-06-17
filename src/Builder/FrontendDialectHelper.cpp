//===--------------------- FrontendDialectHelper.cpp ----------------------===//
//
// Copyright 2019 The IBM Research Authors.
//
// =============================================================================
//
// Helper methods for handling input ONNX models.
//
//===----------------------------------------------------------------------===//
#include <llvm/Support/Endian.h>
#include <llvm/Support/SwapByteOrder.h>

#include "src/Builder/FrontendDialectHelper.hpp"

namespace onnx_mlir {

void replaceAll(
    std::string &str, const std::string &from, const std::string &to) {
  if (from.empty())
    return;
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length(); // In case 'to' contains 'from', like replacing
                              // 'x' with 'yx'
  }
}

std::string legalize_name(std::string name) {
  std::replace(name.begin(), name.end(), '/', '_');
  std::replace(name.begin(), name.end(), '-', '_');
  replaceAll(name, ":", "_colon_");
  // If tensor name starts with a number, prepend n to make it a legal c++
  // identifier.
  if (name.size() > 0 && isdigit(name.at(0)))
    name.insert(0, 1, 'n');
  return name;
}

mlir::Value OnnxMlirSymbolMapping::GetTensorByOnnxName(
    const std::string &name) {
  assert(onnx_name2onnx_mlir_tensor.find(legalize_name(name)) !=
             onnx_name2onnx_mlir_tensor.end() &&
         "Tensor not found");
  return onnx_name2onnx_mlir_tensor.at(legalize_name(name));
}

void OnnxMlirSymbolMapping::AddMapping(
    const std::string &name, mlir::Value tensor) {
  assert(onnx_name2onnx_mlir_tensor.count(legalize_name(name)) == 0 &&
         "Tensor already exists.");
  onnx_name2onnx_mlir_tensor.emplace(legalize_name(name), tensor);
}

bool OnnxMlirSymbolMapping::ContainKey(std::string name) {
  return onnx_name2onnx_mlir_tensor.count(name) != 0;
}

template <typename T>
struct TransformValueToONNXData {
  static const google::protobuf::RepeatedField<T> data(
      onnx::TensorProto initializer) {
    return google::protobuf::RepeatedField<T>();
  }
};

template <>
struct TransformValueToONNXData<double> {
  static const google::protobuf::RepeatedField<double> data(
      onnx::TensorProto initializer) {
    return initializer.double_data();
  }
};

template <>
struct TransformValueToONNXData<float> {
  static const google::protobuf::RepeatedField<float> data(
      onnx::TensorProto initializer) {
    return initializer.float_data();
  }
};

template <>
struct TransformValueToONNXData<int32_t> {
  static const google::protobuf::RepeatedField<int32_t> data(
      onnx::TensorProto initializer) {
    return initializer.int32_data();
  }
};

template <>
struct TransformValueToONNXData<int64_t> {
  static const google::protobuf::RepeatedField<int64_t> data(
      onnx::TensorProto initializer) {
    return initializer.int64_data();
  }
};

template <>
struct TransformValueToONNXData<uint8_t> {
  static const google::protobuf::RepeatedField<int32_t> data(
      onnx::TensorProto initializer) {
    return initializer.int32_data();
  }
};

template <>
struct TransformValueToONNXData<int8_t> {
  static const google::protobuf::RepeatedField<int32_t> data(
      onnx::TensorProto initializer) {
    return initializer.int32_data();
  }
};

template <>
struct TransformValueToONNXData<bool> {
  static const google::protobuf::RepeatedField<int32_t> data(
      onnx::TensorProto initializer) {
    return initializer.int32_data();
  }
};

// Helper method for constructing an array attribute from a model input.
template <typename T>
static std::vector<T> CreateArrayAttribute(onnx::TensorProto initializer) {
  size_t size;
  // printf("CreateArrayAttribute with data size %d\n", initializer.raw_data().size());
  if (initializer.raw_data().size()) {
    // copy & take care of endianness
    std::vector<char> byteInitializer;
    std::copy(initializer.raw_data().begin(), initializer.raw_data().end(),
        back_inserter(byteInitializer));
    size = initializer.raw_data().size() / sizeof(T);
    T *arrayPtr = reinterpret_cast<T *>(&byteInitializer[0]);
    auto array = std::vector<T>(arrayPtr, arrayPtr + size);
    // Perform byte swap if system endianness is BE.
    // ONNX tensor content raw data is always in LE.
    if (llvm::support::endian::system_endianness() !=
        llvm::support::endianness::little)
      for (int i = 0; i < array.size(); i++)
        llvm::sys::swapByteOrder<T>(array[i]);

    return array;
  }

  // copy, no need to take care of endianness
  auto data = TransformValueToONNXData<T>::data(initializer);
  size = data.size();

  if (size > 0) {
    return std::vector<T>(&data[0], &data[0] + size);
  } else {
    return std::vector<T>();
  }
}

// Helper method for construction an array attribute from a model input for
// FP16 data types.
static std::vector<APFloat> CreateArrayAttributeFP16(
    onnx::TensorProto initializer) {
  // Use the uint16_t version to take care of endianness
  auto vals = CreateArrayAttribute<uint16_t>(initializer);

  std::vector<APFloat> floatVals(
      vals.size(), APFloat(APFloat::IEEEhalf(), APInt(16, 0)));

  for (uint32_t i = 0; i < vals.size(); ++i) {
    floatVals[i] = APFloat(APFloat::IEEEhalf(), APInt(16, vals[i]));
  }
  return floatVals;
}

void InitializedTensorMapping::AddMapping(
    std::string name, onnx::TensorProto tensor) {
  assert(nameToInitializedTensor.count(name) == 0 &&
         "Tensor initializer already mapped.");
  nameToInitializedTensor.emplace(name, tensor);
}

bool InitializedTensorMapping::ContainKey(std::string name) {
  return nameToInitializedTensor.count(name) != 0;
}

mlir::Value InitializedTensorMapping::EmitInitializerForInputTensor(
    mlir::Location loc, mlir::OpBuilder &builder, const std::string &name) {
  // Initializer for input.
  onnx::TensorProto initializer = GetInitializedTensor(name);

  // Emit ConstantOp and record the mapping between the input and
  // the constant value.
  // Create value attribute.
  mlir::DenseElementsAttr denseElmAttr =
      onnxTensorProtoToDenseElmAttr(builder, initializer);

  // Create ConstantOp for dense array.
  return builder.create<mlir::ONNXConstantOp>(
      loc, denseElmAttr.getType(), nullptr, denseElmAttr);
}

mlir::DenseElementsAttr onnxTensorProtoToDenseElmAttr(
    mlir::OpBuilder &builder, const onnx::TensorProto &initializer) {
  // Tensor dimensions.
  llvm::ArrayRef<int64_t> tensorDims(
      initializer.dims().data(), initializer.dims().size());
  mlir::DenseElementsAttr denseElmAttr;
  switch (initializer.data_type()) {
  case (onnx::TensorProto::FLOAT16): {
    const auto &arrayAttrInitializer = CreateArrayAttributeFP16(initializer);
    auto elmType = builder.getF16Type();
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);
    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(arrayAttrInitializer));
    break;
  }
  case (onnx::TensorProto::FLOAT): {
    const auto &arrayAttrInitializer = CreateArrayAttribute<float>(initializer);
    auto elmType = builder.getF32Type();
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);
    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(arrayAttrInitializer));
    break;
  }
  case (onnx::TensorProto::DOUBLE): {
    const auto &arrayAttrInitializer =
        CreateArrayAttribute<double>(initializer);
    auto elmType = builder.getF64Type();
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);
    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(arrayAttrInitializer));
    break;
  }
  case (onnx::TensorProto::INT8): {
    const auto &arrayAttrInitializer =
        CreateArrayAttribute<int8_t>(initializer);
    auto elmType = builder.getIntegerType(8);
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);
    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(arrayAttrInitializer));
    break;
  }
  case (onnx::TensorProto::UINT8): {
    const auto &arrayAttrInitializer =
        CreateArrayAttribute<uint8_t>(initializer);
    auto elmType = builder.getIntegerType(8, false);
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);
    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(arrayAttrInitializer));
    break;
  }
  case (onnx::TensorProto::INT32): {
    const auto &arrayAttrInitializer =
        CreateArrayAttribute<int32_t>(initializer);
    auto elmType = builder.getIntegerType(32);
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);
    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(arrayAttrInitializer));
    break;
  }
  case (onnx::TensorProto::INT64): {
    const auto &arrayAttrInitializer =
        CreateArrayAttribute<int64_t>(initializer);
    auto elmType = builder.getIntegerType(64);
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);
    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(arrayAttrInitializer));
    break;
  }
  case (onnx::TensorProto::BOOL): {
    const auto &arrayAttrInitializer =
        CreateArrayAttribute<char>(initializer); // bool is 1 byte rather than 1 bit, may want to define a custom template
    auto elmType = builder.getI1Type(); // equivalent to getIntegerType(1)
    auto tensorType = mlir::RankedTensorType::get(tensorDims, elmType);

    auto raw_data = initializer.raw_data();
    auto size = raw_data.size();
    printf("raw data size is %d\n", size);

    // std::vector<bool> array;
    bool* array = new bool[size]; 
    memset(array, false, (size)*sizeof(bool));

    std::vector<char> byteInitializer;
    std::copy(initializer.raw_data().begin(), initializer.raw_data().end(),
        back_inserter(byteInitializer));
    for (int i = 0; i < size; i++) {
      if (byteInitializer[i]) {
        array[i] = true;
        printf("%d\n", (int)byteInitializer[i]);
      } 
    }
    
    // size = initializer.raw_data().size() / ;
    // T *arrayPtr = reinterpret_cast<T *>(&byteInitializer[0]);
    // auto array = std::vector<T>(arrayPtr, arrayPtr + size);

    denseElmAttr = mlir::DenseElementsAttr::get(
        tensorType, llvm::makeArrayRef(array,size));
    break;
  }
  default:
    llvm_unreachable(
        "Failed to import ONNX TensorProto due to unsupported data types.");
  }
  return denseElmAttr;
}

} // namespace onnx_mlir
