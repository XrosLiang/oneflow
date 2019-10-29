#include "oneflow/xrt/xla/op_context.h"
#include "oneflow/xrt/xla/ops/op_compiler.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"

namespace oneflow {
namespace xrt {
namespace mola {

class BiasAddOp : public OpCompiler {
 public:
  void Compile(OpContext *ctx) override {
    Shape in_shape = ctx->InputShape("a");
    Shape bias_shape = ctx->InputShape("b");
    CHECK_GE(in_shape.NumAxes(), 2);
    CHECK_EQ(bias_shape.NumAxes(), 1);

    xla::XlaOp in = ctx->Input("a");
    xla::XlaOp bias = ctx->Input("b");

    // Channel dim for NCHW data formart
    int channel_dim = 1;
    ctx->SetOutput("out", xla::Add(in, bias, {channel_dim}));
  }
};

REGISTER_XLA_OP_COMPILER(BiasAdd, BiasAddOp).Finalize();

}  // namespace mola
}  // namespace xrt
}  // namespace oneflow