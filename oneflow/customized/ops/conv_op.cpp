#include "oneflow/core/framework/framework.h"
#include "oneflow/customized/ops/nn_util.h"

namespace oneflow {

namespace {

template<size_t NDims>
Maybe<void> InferTensorDesc4Conv(user_op::InferContext* ctx) {
  const user_op::TensorDesc* in = ctx->TensorDesc4ArgNameAndIndex("in", 0);
  CHECK_EQ(NDims + 2, in->shape().NumAxes());

  auto data_format = ctx->GetAttr<std::string>("data_format");
  auto kernel_size = ctx->GetAttr<std::vector<int32_t>>("kernel_size");
  CHECK_EQ_OR_RETURN(NDims, kernel_size.size());
  int32_t filters = ctx->GetAttr<int32_t>("filters");
  size_t idx_offset = IdxOffset(data_format);

  // only support data parallel
  CHECK_OR_RETURN(ctx->parallel_ctx().parallel_num() == 1
                  || ctx->SbpParallel4ArgNameAndIndex("weight", 0).has_broadcast_parallel());

  {
    auto padding = ctx->GetAttr<std::string>("padding");
    auto dilation_rate = ctx->GetAttr<std::vector<int32_t>>("dilation_rate");
    auto strides = ctx->GetAttr<std::vector<int32_t>>("strides");
    CHECK_EQ_OR_RETURN(NDims, dilation_rate.size());
    CHECK_EQ_OR_RETURN(NDims, strides.size());

    user_op::TensorDesc* out = ctx->TensorDesc4ArgNameAndIndex("out", 0);
    DimVector out_shape(NDims + 2);
    out_shape.at(0) = in->shape().At(0);
    const size_t c_dim = data_format == "channels_first" ? 1 : NDims + 1;
    out_shape.at(c_dim) = filters;
    for (int32_t i = 0; i < NDims; ++i) {
      CalcOutAndPadding(in->shape().At(idx_offset + i), kernel_size.at(i), dilation_rate.at(i),
                        strides.at(i), padding, &out_shape.at(idx_offset + i), nullptr, nullptr);
    }
    *out = *in;
    *out->mut_shape() = Shape(out_shape);
  }

  {
    int32_t groups = ctx->GetAttr<int32_t>("groups");
    CHECK_GT_OR_RETURN(groups, 0);
    CHECK_LE_OR_RETURN(groups, filters);
    CHECK_EQ_OR_RETURN(filters % groups, 0);

    DimVector weight_shape(in->shape().dim_vec());
    weight_shape.at(0) = filters;
    if (data_format == "channels_first") {
      CHECK_LE_OR_RETURN(groups, weight_shape.at(1));
      CHECK_EQ_OR_RETURN(weight_shape.at(1) % groups, 0);
      weight_shape.at(1) = weight_shape.at(1) / groups;
    } else if (data_format == "channels_last") {
      CHECK_LE_OR_RETURN(groups, weight_shape.at(NDims + 1));
      CHECK_EQ_OR_RETURN(weight_shape.at(NDims + 1) % groups, 0);
      weight_shape.at(NDims + 1) = weight_shape.at(NDims + 1) / groups;
    } else {
      UNIMPLEMENTED_THEN_RETURN();
    }
    for (size_t i = 0; i < NDims; ++i) { weight_shape.at(idx_offset + i) = kernel_size.at(i); }

    const user_op::TensorDesc* weight = ctx->TensorDesc4ArgNameAndIndex("weight", 0);
    CHECK_EQ(weight->shape(), Shape(weight_shape));
  }

  const user_op::TensorDesc* bias = ctx->TensorDesc4ArgNameAndIndex("bias", 0);
  if (bias != nullptr) { CHECK_EQ_OR_RETURN(bias->shape(), Shape({filters})); }
  return Maybe<void>::Ok();
}

Maybe<void> InferBatchAxis4Conv(user_op::BatchAxisContext* ctx) {
  *ctx->BatchAxis4ArgNameAndIndex("out", 0) = *ctx->BatchAxis4ArgNameAndIndex("in", 0);
  return Maybe<void>::Ok();
}

Maybe<void> GetSbpSignatures4Conv(user_op::SbpContext* ctx) {
  // TODO(niuchong) : handle bias_multiplier
  bool has_bias = false;
  for (const auto& pair : ctx->inputs()) {
    if (pair.first == "bias") {
      CHECK_EQ_OR_RETURN(0, pair.second);
      has_bias = true;
      break;
    }
  }

  if (has_bias) {
    ctx->NewBuilder()
        .Split(user_op::OpArg("in", 0), 0)
        .Broadcast(user_op::OpArg("weight", 0))
        .Broadcast(user_op::OpArg("bias", 0))
        .Split(user_op::OpArg("out", 0), 0)
        .Build();
  } else {
    ctx->NewBuilder()
        .Split(user_op::OpArg("in", 0), 0)
        .Broadcast(user_op::OpArg("weight", 0))
        .Split(user_op::OpArg("out", 0), 0)
        .Build();
  }
  return Maybe<void>::Ok();
}

template<size_t NDims>
Maybe<void> CheckAttr(const user_op::UserOpDefWrapper& def,
                      const user_op::UserOpConfWrapper& conf) {
  bool is_checked = true;
  std::stringstream err;
  err << "Illegal value for " << conf.op_type_name() << " op " << conf.op_name() << ": ";

  const auto& data_format = conf.attr<std::string>("data_format");
  if (!(data_format == "channels_first" || data_format == "channels_last")) {
    err << " data_format:" << data_format;
    is_checked = false;
  }

  const auto& padding = conf.attr<std::string>("padding");
  if (!(padding == "valid" || padding == "same")) {
    err << " padding:" << padding;
    is_checked = false;
  }

  if (NDims != 0) {
    const auto& kernel_size = conf.attr<std::vector<int32_t>>("kernel_size");
    if (kernel_size.size() != NDims) {
      err << " kernel_size: number of element is " << kernel_size.size();
      is_checked = false;
    }

    const auto& strides = conf.attr<std::vector<int32_t>>("strides");
    if (strides.size() != NDims) {
      err << " strides: number of element is " << strides.size();
      is_checked = false;
    }

    const auto& dilation_rate = conf.attr<std::vector<int32_t>>("dilation_rate");
    if (dilation_rate.size() != NDims) {
      err << " dilation_rate: number of element is " << dilation_rate.size();
      is_checked = false;
    }
  }

  if (is_checked) {
    return Maybe<void>::Ok();
  } else {
    return oneflow::Error::CheckFailed() << err.str();
  }
}

void GenerateBackwardOpConf4Conv(const user_op::UserOpWrapper& op, user_op::AddOpFn AddOp) {
  std::string padding = op.attr<std::string>("padding");
  std::string data_format = op.attr<std::string>("data_format");
  std::vector<int32_t> kernel_size = op.attr<std::vector<int32_t>>("kernel_size");
  std::vector<int32_t> strides = op.attr<std::vector<int32_t>>("strides");
  std::vector<int32_t> dilation_rate = op.attr<std::vector<int32_t>>("dilation_rate");
  int32_t groups = op.attr<int32_t>("groups");

  int32_t ndims = kernel_size.size();
  CHECK_EQ(ndims, strides.size());
  CHECK_EQ(ndims, dilation_rate.size());

  if (op.user_op_conf().has_input("bias", 0)) {
    if (op.NeedGenGradTensor4OpInput("bias", 0)) {
      auto bias_grad_op =
          user_op::UserOpConfWrapperBuilder("System-AutoGrad-" + op.op_name() + "-BiasGrad")
              .Op("conv_bias_grad")
              .Input("dy", op.GetGradTensorWithOpOutput("out", 0))
              .Output("bias_diff")
              .Attr<std::string>("data_format", data_format)
              .Attr<int32_t>("num_spatial_dims", ndims)
              .Build();
      op.BindGradTensorWithOpInput(bias_grad_op.output("bias_diff", 0), "bias", 0);
      AddOp(bias_grad_op);
    }
  }

  if (op.NeedGenGradTensor4OpInput("weight", 0)) {
    auto filter_grad_op =
        user_op::UserOpConfWrapperBuilder("System-AutoGrad-" + op.op_name() + "-FilterGrad")
            .Op("conv_filter_grad")
            .Input("dy", op.GetGradTensorWithOpOutput("out", 0))
            .Input("x", op.input("in", 0))
            .Output("filter_diff")
            .Attr<int32_t>("num_spatial_dims", ndims)
            .Attr<std::string>("padding", padding)
            .Attr<std::string>("data_format", data_format)
            .Attr<std::vector<int32_t>>("kernel_size", kernel_size)
            .Attr<std::vector<int32_t>>("strides", strides)
            .Attr<std::vector<int32_t>>("dilation_rate", dilation_rate)
            .Attr<int32_t>("groups", groups)
            .Build();
    op.BindGradTensorWithOpInput(filter_grad_op.output("filter_diff", 0), "weight", 0);
    AddOp(filter_grad_op);
  }

  if (op.NeedGenGradTensor4OpInput("in", 0)) {
    auto data_grad_op =
        user_op::UserOpConfWrapperBuilder("System-AutoGrad-" + op.op_name() + "-DataGrad")
            .Op("conv_data_grad")
            .Input("dy", op.GetGradTensorWithOpOutput("out", 0))
            .Input("filter", op.input("weight", 0))
            .Input("x_like", op.input("in", 0))
            .Output("dx")
            .Attr<int32_t>("num_spatial_dims", ndims)
            .Attr<std::string>("padding", padding)
            .Attr<std::string>("data_format", data_format)
            .Attr<std::vector<int32_t>>("kernel_size", kernel_size)
            .Attr<std::vector<int32_t>>("strides", strides)
            .Attr<std::vector<int32_t>>("dilation_rate", dilation_rate)
            .Attr<int32_t>("groups", groups)
            .Build();
    op.BindGradTensorWithOpInput(data_grad_op.output("dx", 0), "in", 0);
    AddOp(data_grad_op);
  }
}

}  // namespace

REGISTER_USER_OP("conv1d")
    .Input("in")
    .Input("weight")
    .OptionalInput("bias")
    .OptionalInput("bias_multiplier")  // cudnn conv doesn't need this
    .Output("out")
    .Attr("filters", UserOpAttrType::kAtInt32)
    .Attr<std::string>("padding", UserOpAttrType::kAtString, "valid")
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr<int32_t>("groups", UserOpAttrType::kAtInt32, 1)
    .SetCheckAttrFn(CheckAttr<1>)
    .SetTensorDescInferFn(InferTensorDesc4Conv<1>)
    .SetBatchAxisInferFn(InferBatchAxis4Conv)
    .SetGetSbpFn(GetSbpSignatures4Conv);

REGISTER_USER_OP("conv2d")
    .Input("in")
    .Input("weight")
    .OptionalInput("bias")
    .OptionalInput("bias_multiplier")  // cudnn conv doesn't need this
    .Output("out")
    .Attr("filters", UserOpAttrType::kAtInt32)
    .Attr<std::string>("padding", UserOpAttrType::kAtString, "valid")
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr<int32_t>("groups", UserOpAttrType::kAtInt32, 1)
    .SetCheckAttrFn(CheckAttr<2>)
    .SetTensorDescInferFn(InferTensorDesc4Conv<2>)
    .SetBatchAxisInferFn(InferBatchAxis4Conv)
    .SetGetSbpFn(GetSbpSignatures4Conv);

REGISTER_USER_OP("conv3d")
    .Input("in")
    .Input("weight")
    .OptionalInput("bias")
    .OptionalInput("bias_multiplier")  // cudnn conv doesn't need this
    .Output("out")
    .Attr("filters", UserOpAttrType::kAtInt32)
    .Attr<std::string>("padding", UserOpAttrType::kAtString, "valid")
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr<int32_t>("groups", UserOpAttrType::kAtInt32, 1)
    .SetCheckAttrFn(CheckAttr<3>)
    .SetTensorDescInferFn(InferTensorDesc4Conv<3>)
    .SetBatchAxisInferFn(InferBatchAxis4Conv)
    .SetGetSbpFn(GetSbpSignatures4Conv);

REGISTER_USER_OP_GRAD("conv1d").SetGenBackwardOpConfFn(GenerateBackwardOpConf4Conv);
REGISTER_USER_OP_GRAD("conv2d").SetGenBackwardOpConfFn(GenerateBackwardOpConf4Conv);
REGISTER_USER_OP_GRAD("conv3d").SetGenBackwardOpConfFn(GenerateBackwardOpConf4Conv);

REGISTER_USER_OP("conv_data_grad")
    .Input("dy")
    .Input("filter")
    .Input("x_like")
    .Output("dx")
    .Attr("num_spatial_dims", UserOpAttrType::kAtInt32)
    .Attr("padding", UserOpAttrType::kAtString)
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr("groups", UserOpAttrType::kAtInt32)
    .SetCheckAttrFn(CheckAttr<0>)
    .SetInputArgModifyFn([](user_op::GetInputArgModifier GetInputArgModifierFn,
                            const user_op::UserOpConfWrapper&) {
      user_op::InputArgModifier* x_like = GetInputArgModifierFn("x_like", 0);
      CHECK_NOTNULL(x_like);
      x_like->set_use_header_only(true);
    })
    .SetTensorDescInferFn([](user_op::InferContext* ctx) -> Maybe<void> {
      const user_op::TensorDesc* dy = ctx->TensorDesc4ArgNameAndIndex("dy", 0);
      const user_op::TensorDesc* x_like = ctx->TensorDesc4ArgNameAndIndex("x_like", 0);
      const int32_t num_spatial_dims = ctx->GetAttr<int32_t>("num_spatial_dims");
      CHECK_GE_OR_RETURN(num_spatial_dims, 1);
      CHECK_LE_OR_RETURN(num_spatial_dims, 3);
      CHECK_EQ_OR_RETURN(dy->shape().NumAxes(), num_spatial_dims + 2);
      CHECK_EQ_OR_RETURN(x_like->shape().NumAxes(), num_spatial_dims + 2);
      CHECK_EQ_OR_RETURN(x_like->data_type(), dy->data_type());

      user_op::TensorDesc* dx = ctx->TensorDesc4ArgNameAndIndex("dx", 0);
      *dx = *x_like;
      return Maybe<void>::Ok();
    })
    .SetBatchAxisInferFn([](user_op::BatchAxisContext* ctx) -> Maybe<void> {
      auto BatchAxis4BnInOp = [&ctx](const std::string& arg_name) -> OptInt64* {
        return ctx->BatchAxis4ArgNameAndIndex(arg_name, 0);
      };
      CHECK_OR_RETURN(*BatchAxis4BnInOp("dy") == *BatchAxis4BnInOp("x_like"));
      CHECK_OR_RETURN(BatchAxis4BnInOp("filter")->has_value() == false);
      *BatchAxis4BnInOp("dx") = *BatchAxis4BnInOp("dy");
      return Maybe<void>::Ok();
    })
    .SetGetSbpFn([](user_op::SbpContext* ctx) -> Maybe<void> {
      ctx->NewBuilder()
          .Split(user_op::OpArg("dy", 0), 0)
          .Broadcast(user_op::OpArg("filter", 0))
          .Split(user_op::OpArg("x_like", 0), 0)
          .Split(user_op::OpArg("dx", 0), 0)
          .Build();
      return Maybe<void>::Ok();
    });

REGISTER_USER_OP("conv_filter_grad")
    .Input("dy")
    .Input("x")
    .Output("filter_diff")
    .Attr("num_spatial_dims", UserOpAttrType::kAtInt32)
    .Attr("padding", UserOpAttrType::kAtString)
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("kernel_size", UserOpAttrType::kAtListInt32)
    .Attr("strides", UserOpAttrType::kAtListInt32)
    .Attr("dilation_rate", UserOpAttrType::kAtListInt32)
    .Attr("groups", UserOpAttrType::kAtInt32)
    .SetCheckAttrFn(CheckAttr<0>)
    .SetTensorDescInferFn([](user_op::InferContext* ctx) -> Maybe<void> {
      const user_op::TensorDesc* dy = ctx->TensorDesc4ArgNameAndIndex("dy", 0);
      const user_op::TensorDesc* x = ctx->TensorDesc4ArgNameAndIndex("x", 0);

      const int32_t num_spatial_dims = ctx->GetAttr<int32_t>("num_spatial_dims");
      const int32_t groups = ctx->GetAttr<int32_t>("groups");
      const std::string& data_format = ctx->GetAttr<std::string>("data_format");
      const std::vector<int32_t> kernel_size = ctx->GetAttr<std::vector<int32_t>>("kernel_size");

      CHECK_GE_OR_RETURN(num_spatial_dims, 1);
      CHECK_LE_OR_RETURN(num_spatial_dims, 3);
      CHECK_EQ_OR_RETURN(dy->shape().NumAxes(), num_spatial_dims + 2);
      CHECK_EQ_OR_RETURN(x->shape().NumAxes(), num_spatial_dims + 2);
      CHECK_EQ_OR_RETURN(x->data_type(), dy->data_type());
      CHECK_GT_OR_RETURN(groups, 0);

      DimVector filter_diff_dim_vec;
      if (data_format == "channels_first") {
        CHECK_LE_OR_RETURN(groups, x->shape().At(1));
        CHECK_LE_OR_RETURN(groups, dy->shape().At(1));
        CHECK_EQ_OR_RETURN(x->shape().At(1) % groups, 0);
        CHECK_EQ_OR_RETURN(dy->shape().At(1) % groups, 0);
        filter_diff_dim_vec.push_back(dy->shape().At(1));
        filter_diff_dim_vec.push_back(x->shape().At(1) / groups);
        filter_diff_dim_vec.insert(filter_diff_dim_vec.end(), kernel_size.cbegin(),
                                   kernel_size.cend());
      } else {
        CHECK_EQ_OR_RETURN("channels_last", data_format);
        CHECK_EQ_OR_RETURN(groups, 1);
        filter_diff_dim_vec.push_back(dy->shape().dim_vec().back());
        filter_diff_dim_vec.insert(filter_diff_dim_vec.end(), kernel_size.cbegin(),
                                   kernel_size.cend());
        filter_diff_dim_vec.push_back(x->shape().dim_vec().back() / groups);
      }

      user_op::TensorDesc* filter_diff = ctx->TensorDesc4ArgNameAndIndex("filter_diff", 0);
      *filter_diff->mut_shape() = Shape(filter_diff_dim_vec);
      *filter_diff->mut_data_type() = x->data_type();

      return Maybe<void>::Ok();
    })
    .SetBatchAxisInferFn([](user_op::BatchAxisContext* ctx) -> Maybe<void> {
      auto BatchAxis4BnInOp = [&ctx](const std::string& arg_name) -> OptInt64* {
        return ctx->BatchAxis4ArgNameAndIndex(arg_name, 0);
      };
      CHECK_OR_RETURN(*BatchAxis4BnInOp("dy") == *BatchAxis4BnInOp("x"));
      BatchAxis4BnInOp("filter_diff")->clear_value();
      return Maybe<void>::Ok();
    })
    .SetGetSbpFn([](user_op::SbpContext* ctx) -> Maybe<void> {
      ctx->NewBuilder()
          .Split(user_op::OpArg("dy", 0), 0)
          .Split(user_op::OpArg("x", 0), 0)
          .PartialSum(user_op::OpArg("filter_diff", 0))
          .Build();
      return Maybe<void>::Ok();
    });

REGISTER_USER_OP("conv_bias_grad")
    .Input("dy")
    .Output("bias_diff")
    .Attr("data_format", UserOpAttrType::kAtString)
    .Attr("num_spatial_dims", UserOpAttrType::kAtInt32)
    .SetCheckAttrFn([](const user_op::UserOpDefWrapper& def,
                       const user_op::UserOpConfWrapper& conf) -> Maybe<void> {
      std::string data_format = conf.attr<std::string>("data_format");
      if (data_format == "channels_first" || data_format == "channels_last") {
        return Maybe<void>::Ok();
      }
      return oneflow::Error::CheckFailed() << "Illegal value for " << conf.op_type_name() << " op "
                                           << conf.op_name() << ": data_format:" << data_format;
    })
    .SetTensorDescInferFn([](user_op::InferContext* ctx) -> Maybe<void> {
      const user_op::TensorDesc* dy = ctx->TensorDesc4ArgNameAndIndex("dy", 0);
      user_op::TensorDesc* bias_diff = ctx->TensorDesc4ArgNameAndIndex("bias_diff", 0);

      int32_t num_spatial_dims = ctx->GetAttr<int32_t>("num_spatial_dims");
      std::string data_format = ctx->GetAttr<std::string>("data_format");

      CHECK_GE_OR_RETURN(num_spatial_dims, 1);
      CHECK_LE_OR_RETURN(num_spatial_dims, 3);
      CHECK_EQ_OR_RETURN(dy->shape().NumAxes(), num_spatial_dims + 2);
      *bias_diff->mut_data_type() = dy->data_type();
      if (data_format == "channels_first") {
        *bias_diff->mut_shape() = Shape({dy->shape().At(1)});
      } else if (data_format == "channels_last") {
        *bias_diff->mut_shape() = Shape({dy->shape().At(dy->shape().NumAxes() - 1)});
      } else {
        OF_UNIMPLEMENTED();
      }
      return Maybe<void>::Ok();
    })
    .SetBatchAxisInferFn([](user_op::BatchAxisContext* ctx) -> Maybe<void> {
      CHECK_OR_RETURN(ctx->BatchAxis4ArgNameAndIndex("dy", 0)->has_value());
      ctx->BatchAxis4ArgNameAndIndex("bias_diff", 0)->clear_value();
      return Maybe<void>::Ok();
    })
    .SetGetSbpFn([](user_op::SbpContext* ctx) -> Maybe<void> {
      ctx->NewBuilder()
          .Split(user_op::OpArg("dy", 0), 0)
          .PartialSum(user_op::OpArg("bias_diff", 0))
          .Build();
      return Maybe<void>::Ok();
    });

}  // namespace oneflow
