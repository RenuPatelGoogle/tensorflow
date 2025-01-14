/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include <utility>

#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"
#include "tensorflow/compiler/mlir/lite/transforms/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops_n_z.h"

namespace mlir {
namespace TFL {
namespace {

// The threshold of constant bits to be unfolded (1Mb). If there is a splat
// constant with size equal or greater to this threshold, then it will be
// unfolded back to a regular `tfl.fill` operation.
constexpr int64_t kConstantSizeThresholdInBits = 1e+6;

// Pass which will replace large splat constant tensors to `tfl.Fill` op to
// reduce the size of the generated flatbuffer model size.
class UnfoldLargeSplatConstant
    : public PassWrapper<UnfoldLargeSplatConstant, OperationPass<ModuleOp>> {
 public:
  void getDependentDialects(DialectRegistry& registry) const override {
    registry.insert<TFL::TensorFlowLiteDialect>();
  }

  StringRef getArgument() const final {
    // This is the argument used to refer to the pass in
    // the textual format (on the commandline for example).
    return "tfl-unfold-large-splat-constant";
  }
  StringRef getDescription() const final {
    // This is a brief description of the pass.
    return "Unfold large splat constant tensors";
  }

  void runOnOperation() override {
    auto module = getOperation();

    mlir::OpBuilder op_builder(&module.body());
    module.walk([&](mlir::arith::ConstantOp const_op) {
      MaybeUnfoldLargeSplatConstant(&op_builder, const_op);
    });
  }

 private:
  void MaybeUnfoldLargeSplatConstant(mlir::OpBuilder* op_builder,
                                     mlir::arith::ConstantOp const_op) const {
    auto splat_elements_attr =
        const_op.getValue().dyn_cast<SplatElementsAttr>();
    if (!splat_elements_attr) {
      return;
    }
    auto element_type = splat_elements_attr.getType().getElementType();
    if (!(element_type.isF32() || element_type.isInteger(1) ||
          element_type.isInteger(32) || element_type.isInteger(64))) {
      return;
    }
    if (splat_elements_attr.getNumElements() *
            splat_elements_attr.getType().getElementTypeBitWidth() <
        kConstantSizeThresholdInBits) {
      return;
    }

    op_builder->setInsertionPoint(const_op);
    mlir::arith::ConstantOp fill_shape =
        op_builder->create<mlir::arith::ConstantOp>(
            const_op->getLoc(),
            DenseIntElementsAttr::get(
                RankedTensorType::get({splat_elements_attr.getType().getRank()},
                                      op_builder->getI64Type()),
                splat_elements_attr.getType().getShape()));
    mlir::arith::ConstantOp fill_value =
        op_builder->create<mlir::arith::ConstantOp>(
            const_op->getLoc(),
            DenseElementsAttr::get(
                RankedTensorType::get(
                    {}, splat_elements_attr.getType().getElementType()),
                splat_elements_attr.getSplatValue()));
    TFL::FillOp fill = op_builder->create<TFL::FillOp>(
        const_op->getLoc(), splat_elements_attr.getType(), fill_shape,
        fill_value);
    const_op->replaceAllUsesWith(fill);
    const_op->erase();
  }
};

}  // namespace

std::unique_ptr<OperationPass<ModuleOp>> CreateUnfoldLargeSplatConstantPass() {
  return std::make_unique<UnfoldLargeSplatConstant>();
}

static PassRegistration<UnfoldLargeSplatConstant> pass;

}  // namespace TFL
}  // namespace mlir
