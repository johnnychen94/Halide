#include "ops.h"
#include "app_util.h"

#include <cmath>
#include <iostream>

#include "AddUint8Uint8.h"
#include "AveragePoolUint8.h"
#include "ConvolutionUint8.h"
#include "DepthwiseConvolutionUint8.h"
#include "DepthwiseConvolutionUint8Broadcast.h"
#include "MaxPoolUint8.h"

namespace interpret_nn {

namespace {

std::vector<Box> split_crop(const Box &crop, int dim, int factor, bool shift_inwards = false) {
    std::vector<Box> splits;
    int x_min = crop[dim].min;
    int x_extent = crop[dim].extent();
    int x_max = x_min + x_extent - 1;
    splits.reserve((x_extent + factor - 1) / factor);
    Box split_x = crop;
    split_x[dim].set_extent(factor);
    for (int x = 0; x <= x_max; x += factor, split_x[dim] += factor) {
        if (shift_inwards) {
            if (split_x[dim].max >= crop[dim].max) {
                split_x[dim] -= split_x[dim].max - crop[dim].max;
            }
            APP_CHECK(split_x[dim].min >= crop[dim].min);
            APP_CHECK(split_x[dim].max <= crop[dim].max);
        } else {
            split_x[dim].max = std::min(split_x[dim].max, crop[dim].max);
        }
        APP_CHECK(split_x[dim].extent() > 0);
        splits.push_back(split_x);
    }
    return splits;
}

struct QuantizedMulAndShift {
    int multiplier, shift;
};

// Adapted from tflite
QuantizedMulAndShift GetQuantizedMulAndShift(double double_multiplier) {
    if (double_multiplier == 0.) {
        return {0, 0};
    }

    // TODO: consider adding a path here to avoid floating-point operations (eg for Hexagon)
    int shift = 0;
    const double q = std::frexp(double_multiplier, &shift);
    int64_t q_fixed = (int64_t)std::round(q * (1LL << 31));
    APP_CHECK(q_fixed <= (1LL << 31));

    if (q_fixed == (1LL << 31)) {
        q_fixed /= 2;
        ++shift;
    }
    APP_CHECK(q_fixed <= std::numeric_limits<int32_t>::max());

    if (shift < -31) {
        shift = 0;
        q_fixed = 0;
    }
    return {(int)q_fixed, shift};
}

// Adapted from tflite
QuantizedMulAndShift GetQuantizedMulAndShiftSmallerThanOne(double double_multiplier) {
    APP_CHECK(double_multiplier > 0.0 && double_multiplier < 1.0);
    auto result = GetQuantizedMulAndShift(double_multiplier);
    APP_CHECK(result.shift <= 0);
    return result;
}

struct MinMax {
    int min, max;
};

// Adapted from tfmini
MinMax GetQuantizedMinMax(ActivationFunction activation, int zero_point, double scale) {
    double real_activation_min = 0.0;
    double real_activation_max = 0.0;
    bool has_activation_min = false;
    bool has_activation_max = false;
    if (activation == ActivationFunction::None) {
        // nothing
    } else if (activation == ActivationFunction::Relu) {
        real_activation_min = 0.0;
        has_activation_min = true;
    } else if (activation == ActivationFunction::Relu6) {
        real_activation_min = 0.0;
        has_activation_min = true;
        real_activation_max = 6.0;
        has_activation_max = true;
    } else if (activation == ActivationFunction::ReluN1To1) {
        real_activation_min = -1.0;
        has_activation_min = true;
        real_activation_max = 1.0;
        has_activation_max = true;
    } else {
        APP_CHECK(false) << "Unsupported quantized activation function type.";
    }
    int output_activation_min = 0;
    int output_activation_max = 255;
    if (has_activation_min) {
        output_activation_min = std::max(
            output_activation_min,
            zero_point + (int)std::round(real_activation_min / scale));
    }
    if (has_activation_max) {
        output_activation_max = std::min(
            output_activation_max,
            zero_point + (int)std::round(real_activation_max / scale));
    }

    return {output_activation_min, output_activation_max};
}

MinMax GetoutRange(ActivationFunction activation, Tensor *out) {
    const int output_offset = out->quantization().zero.at(0);
    APP_CHECK(output_offset >= 0 && output_offset <= 255);

    const float output_scale = out->quantization().scale.at(0);

    const auto output_range = GetQuantizedMinMax(activation, output_offset, output_scale);
    // TODO: handle unexpected out-of-range data more cleanly.
    APP_CHECK(output_range.min >= 0 && output_range.min <= 255);
    APP_CHECK(output_range.max >= 0 && output_range.max <= 255);
    APP_CHECK(output_range.min <= output_range.max);

    return output_range;
}

}  // namespace

Op::Bounds ElementwiseOp::infer_bounds(const Box &crop) const {
    Bounds result;
    for (int i = 0; i < input_count(); i++) {
        result.inputs.emplace_back(crop);
    }
    for (int i = 0; i < output_count(); i++) {
        result.outputs.emplace_back(crop);
    }
    return result;
}

std::vector<Box> ElementwiseOp::split(const Box &crop) const {
    const int ksplit = 2;
    return split_crop(crop, 2, ksplit);
}

Op::Bounds PoolOp::infer_bounds(const Box &crop) const {
    Box input_crop = crop;

    input_crop[0] = crop[0];
    for (int dim = 1; dim <= 2; dim++) {
        input_crop[dim] *= stride_[dim - 1];
    }

    input_crop[1].max += filter_size_[0] - 1;
    input_crop[2].max += filter_size_[1] - 1;
    input_crop = intersect(input_crop, without_strides(input()->shape()));

    Bounds result;
    result.inputs.emplace_back(input_crop);
    result.outputs = {crop};
    return result;
}

std::vector<Box> PoolOp::split(const Box &crop) const {
    const int ksplit = 2;
    return split_crop(crop, 2, ksplit);
}

void AddOp::execute(const Box &crop) {
    const Tensor *in1 = input(0);
    const Tensor *in2 = input(1);
    Tensor *out = output();

    if (in1->type() == TensorType::UInt8 &&
        in2->type() == TensorType::UInt8 &&
        out->type() == TensorType::UInt8) {
        auto in1_buf = in1->data<uint8_t>();
        auto in2_buf = in2->data<uint8_t>();
        auto output_buf = out->data<uint8_t>(crop);

        const int in1_offset = in1->quantization().zero.at(0);
        const int in2_offset = in2->quantization().zero.at(0);
        const int output_offset = out->quantization().zero.at(0);
        APP_CHECK(in1_offset >= 0 && in1_offset <= 255);
        APP_CHECK(in2_offset >= 0 && in2_offset <= 255);
        APP_CHECK(output_offset >= 0 && output_offset <= 255);

        const float in1_scale = in1->quantization().scale.at(0);
        const float in2_scale = in2->quantization().scale.at(0);
        const float output_scale = out->quantization().scale.at(0);

        const int left_shift = 20;  // 20 for 8-bit, 15 for 16-bit
        const double twice_max_input_scale = 2 * std::max(in1_scale, in2_scale);
        const double real_in1_multiplier = in1_scale / twice_max_input_scale;
        const double real_in2_multiplier = in2_scale / twice_max_input_scale;
        const double real_output_multiplier = twice_max_input_scale / ((1 << left_shift) * output_scale);

        const auto in1_mul_and_shift = GetQuantizedMulAndShiftSmallerThanOne(real_in1_multiplier);
        const auto in2_mul_and_shift = GetQuantizedMulAndShiftSmallerThanOne(real_in2_multiplier);
        const auto output_mul_and_shift = GetQuantizedMulAndShiftSmallerThanOne(real_output_multiplier);
        APP_CHECK(in1_mul_and_shift.shift <= 0);
        APP_CHECK(in2_mul_and_shift.shift <= 0);
        APP_CHECK(output_mul_and_shift.shift <= 0);

        // TODO: for SubOp:
        // mul_and_shift2.multiplier *= -1;

        const auto output_range = GetoutRange(activation_, out);

        APP_CHECK(0 == AddUint8Uint8(left_shift, in1_buf, in2_buf,
                                     -in1_offset, in1_mul_and_shift.multiplier, -in1_mul_and_shift.shift,
                                     -in2_offset, in2_mul_and_shift.multiplier, -in2_mul_and_shift.shift,
                                     output_offset, output_mul_and_shift.multiplier, -output_mul_and_shift.shift,
                                     output_range.min, output_range.max, output_buf));
    }
}

void AveragePoolOp::execute(const Box &crop) {
    const Tensor *in = input();
    Tensor *out = output();

    if (in->type() == TensorType::UInt8 &&
        out->type() == TensorType::UInt8) {
        auto input_buf = in->data<uint8_t>();
        auto output_buf = out->data<uint8_t>(crop);

        const auto output_range = GetoutRange(activation_, out);

        APP_CHECK(
            0 == AveragePoolUint8(input_buf, stride_[0], stride_[1],
                                  filter_size_[0], filter_size_[1],
                                  output_range.min, output_range.max, output_buf));
    }
}

Op::Bounds ConcatenationOp::infer_bounds(const Box &crop) const {
    // We need everything from the concatenated dimension, everything else
    // is the same as the crop.
    // TODO: It's possible that if the concatenated dimension is cropped
    // from the out, we could reduce the bounds required of some of the
    // ins.
    Bounds result;
    for (int i = 0; i < input_count(); i++) {
        result.inputs.emplace_back(crop);
        result.inputs.back()[axis_] = input(i)->dim(axis_);
    }
    result.outputs.emplace_back(crop);
    result.outputs.back()[axis_] = output()->dim(axis_);
    return result;
}

std::vector<Box> ConcatenationOp::split(const Box &crop) const {
    assert(axis_ != 2);
    // split this into individual lines, so it can get re-fused with any
    // alignment.
    const int ksplit = 1;
    return split_crop(crop, 2, ksplit);
}

void ConcatenationOp::execute(const Box &crop) {
    Tensor *out = output();

    auto output_buf = out->data<void>(crop);

    int output_i = output_buf.dim(axis_).min();
    for (int i = 0; i < input_count(); i++) {
        HalideBuffer<void> input_buf = input(i)->data<void>(crop);
        for (int j = input_buf.dim(axis_).min(); j <= input_buf.dim(axis_).max(); j++) {
            // TODO: Maybe we could just copy whole buffers?
            HalideBuffer<void> input_j = input_buf.sliced(axis_, j);
            HalideBuffer<void> output_j = output_buf.sliced(axis_, output_i++);
            output_j.copy_from(input_j);
        }
    }
}

Op::Bounds Conv2DOp::infer_bounds(const Box &crop) const {
    Box input_crop = crop;
    Box filter_shape = without_strides(filter()->shape());

    for (int dim = 1; dim <= 2; dim++) {
        input_crop[dim] *= stride_[dim - 1];
    }

    input_crop[0] = filter_shape[3];
    input_crop[1].max += dilation_[0] * (filter_shape[1].extent() - 1);
    input_crop[2].max += dilation_[1] * (filter_shape[2].extent() - 1);
    input_crop = intersect(input_crop, without_strides(input()->shape()));

    if (padding_ == Padding::Same) {
        const int input_width = input()->dim(1).extent;
        const int input_height = input()->dim(2).extent;
        const int filter_width = filter()->dim(1).extent;
        const int filter_height = filter()->dim(2).extent;
        const int output_width = output()->dim(1).extent;
        const int output_height = output()->dim(2).extent;

        const int dilated_filter_width = dilation_[0] * (filter_width - 1) + 1;
        const int dilated_filter_height = dilation_[1] * (filter_height - 1) + 1;

        const int pad_width = std::max(0,
                                       ((output_width - 1) * stride_[0] + dilated_filter_width - input_width) / 2);
        const int pad_height = std::max(0,
                                        ((output_height - 1) * stride_[1] + dilated_filter_height - input_height) / 2);

        input_crop[1] += pad_width;
        input_crop[2] += pad_height;
    }

    Bounds result;
    result.inputs.emplace_back(input_crop);
    result.inputs.emplace_back(std::move(filter_shape));
    result.inputs.emplace_back(without_strides(bias()->shape()));
    result.outputs = {crop};

    return result;
}

std::vector<Box> Conv2DOp::split(const Box &crop) const {
    const int ksplit = 2;
    return split_crop(crop, 2, ksplit);
}

void Conv2DOp::execute(const Box &crop) {
    const Tensor *in = input();
    const Tensor *filt = filter();
    Tensor *out = output();

    if (in->type() == TensorType::UInt8 &&
        filt->type() == TensorType::UInt8 &&
        out->type() == TensorType::UInt8) {
        // TODO: reduce code duplication between here and DepthwiseConv2D
        auto input_buf = in->data<uint8_t>();
        auto filter_buf = filt->data<uint8_t>();
        auto bias_buf = bias()->data<int32_t>();
        auto output_buf = out->data<uint8_t>(crop);

        const int input_offset = in->quantization().zero.at(0);
        const int filter_offset = filt->quantization().zero.at(0);
        const int bias_offset = bias()->quantization().zero.at(0);
        const int output_offset = out->quantization().zero.at(0);

        // TODO: handle unexpected out-of-range data more cleanly.
        APP_CHECK(input_offset >= 0 && input_offset <= 255);
        APP_CHECK(filter_offset >= 0 && filter_offset <= 255);
        APP_CHECK(bias_offset == 0);
        APP_CHECK(output_offset >= 0 && output_offset <= 255);

        const float input_scale = in->quantization().scale.at(0);
        const float filter_scale = filt->quantization().scale.at(0);
        const float bias_scale = bias()->quantization().scale.at(0);
        const float output_scale = out->quantization().scale.at(0);

        const double input_product_scale = input_scale * filter_scale;
        // TODO: handle unexpected out-of-range data more cleanly.
        APP_CHECK(std::abs(input_product_scale - bias_scale) <=
                  std::min(input_product_scale, (double)bias_scale) * 1e-6);

        const double real_multiplier = input_product_scale / output_scale;
        const auto mul_and_shift = GetQuantizedMulAndShiftSmallerThanOne(real_multiplier);
        const int output_multiplier = mul_and_shift.multiplier;
        // GetQuantizedMulAndShiftSmallerThanOne() returns a negative shift;
        // ConvolutionUint8() expects a positive shift.
        const int output_shift = -mul_and_shift.shift;

        const auto output_range = GetoutRange(activation_, out);

        if (padding_ == Padding::Same) {
            const int input_width = input_buf.dim(1).extent();
            const int input_height = input_buf.dim(2).extent();
            const int filter_width = filter_buf.dim(1).extent();
            const int filter_height = filter_buf.dim(2).extent();
            const int output_width = output_buf.dim(1).extent();
            const int output_height = output_buf.dim(2).extent();

            const int dilated_filter_width = dilation_[0] * (filter_width - 1) + 1;
            const int dilated_filter_height = dilation_[1] * (filter_height - 1) + 1;

            const int pad_width = std::max(0,
                                           ((output_width - 1) * stride_[0] + dilated_filter_width - input_width) / 2);
            const int pad_height = std::max(0,
                                            ((output_height - 1) * stride_[1] + dilated_filter_height - input_height) / 2);

            input_buf.translate({0, pad_width, pad_height, 0});
        }

        APP_CHECK(
            0 == ConvolutionUint8(input_buf, filter_buf, bias_buf, (uint8_t)input_offset,
                                  (uint8_t)filter_offset, stride_[0], stride_[1],
                                  dilation_[0], dilation_[1], output_multiplier,
                                  output_shift, (uint8_t)output_offset,
                                  output_range.min, output_range.max, output_buf));
    }
}

Op::Bounds DepthwiseConv2DOp::infer_bounds(const Box &crop) const {
    Box input_crop = crop;
    Box filter_shape = without_strides(filter()->shape());

    input_crop[0] = crop[0];
    input_crop[0] /= depth_multiplier_;
    for (int dim = 1; dim <= 2; dim++) {
        input_crop[dim] *= stride_[dim - 1];
    }

    input_crop[1].max += dilation_[0] * (filter_shape[1].extent() - 1);
    input_crop[2].max += dilation_[1] * (filter_shape[2].extent() - 1);
    input_crop = intersect(input_crop, without_strides(input()->shape()));

    if (padding_ == Padding::Same) {
        const int input_width = input()->dim(1).extent;
        const int input_height = input()->dim(2).extent;
        const int filter_width = filter()->dim(1).extent;
        const int filter_height = filter()->dim(2).extent;
        const int output_width = output()->dim(1).extent;
        const int output_height = output()->dim(2).extent;

        const int dilated_filter_width = dilation_[0] * (filter_width - 1) + 1;
        const int dilated_filter_height = dilation_[1] * (filter_height - 1) + 1;

        const int pad_width = std::max(0,
                                       ((output_width - 1) * stride_[0] + dilated_filter_width - input_width) / 2);
        const int pad_height = std::max(0,
                                        ((output_height - 1) * stride_[1] + dilated_filter_height - input_height) / 2);

        input_crop[1] += pad_width;
        input_crop[2] += pad_height;
    }

    Bounds result;
    result.inputs.emplace_back(input_crop);
    result.inputs.emplace_back(std::move(filter_shape));
    result.inputs.emplace_back(without_strides(bias()->shape()));
    result.outputs = {crop};
    return result;
}

std::vector<Box> DepthwiseConv2DOp::split(const Box &crop) const {
    const int ksplit = 2;
    return split_crop(crop, 2, ksplit, true);
}

void DepthwiseConv2DOp::execute(const Box &crop) {
    const Tensor *in = input();
    const Tensor *filt = filter();
    Tensor *out = output();

    if (in->type() == TensorType::UInt8 &&
        filt->type() == TensorType::UInt8 &&
        out->type() == TensorType::UInt8) {
        // TODO: reduce code duplication between here and Conv2D
        auto input_buf = in->data<uint8_t>();
        auto filter_buf = filt->data<uint8_t>().sliced(3, 0);
        auto bias_buf = bias()->data<int32_t>();
        auto output_buf = out->data<uint8_t>(crop);

        int depth_multiplier = output_buf.dim(0).extent() / input_buf.dim(0).extent();
        // TODO: handle unexpected out-of-range data more cleanly.
        APP_CHECK(depth_multiplier * input_buf.dim(0).extent() == output_buf.dim(0).extent());

        const int input_offset = in->quantization().zero.at(0);
        const int filter_offset = filt->quantization().zero.at(0);
        const int bias_offset = bias()->quantization().zero.at(0);
        const int output_offset = out->quantization().zero.at(0);

        // TODO: handle unexpected out-of-range data more cleanly.
        APP_CHECK(input_offset >= 0 && input_offset <= 255);
        APP_CHECK(filter_offset >= 0 && filter_offset <= 255);
        APP_CHECK(bias_offset == 0);
        APP_CHECK(output_offset >= 0 && output_offset <= 255);

        const float input_scale = in->quantization().scale.at(0);
        const float filter_scale = filt->quantization().scale.at(0);
        const float bias_scale = bias()->quantization().scale.at(0);
        const float output_scale = out->quantization().scale.at(0);

        const double input_product_scale = input_scale * filter_scale;
        // TODO: handle unexpected out-of-range data more cleanly.
        APP_CHECK(std::abs(input_product_scale - bias_scale) <=
                  std::min(input_product_scale, (double)bias_scale) * 1e-6);

        const double real_multiplier = input_product_scale / output_scale;
        const auto mul_and_shift = GetQuantizedMulAndShiftSmallerThanOne(real_multiplier);
        const int output_multiplier = mul_and_shift.multiplier;
        // GetQuantizedMulAndShiftSmallerThanOne() returns a negative shift;
        // DepthwiseConvolutionUint8() expects a positive shift.
        const int output_shift = -mul_and_shift.shift;

        const auto output_range = GetoutRange(activation_, out);

        // batches must match
        APP_CHECK(input_buf.dim(3).extent() == output_buf.dim(3).extent());

        // output_depth must match
        APP_CHECK(filter_buf.dim(0).extent() == output_buf.dim(0).extent());

        if (padding_ == Padding::Same) {
            const int input_width = input_buf.dim(1).extent();
            const int input_height = input_buf.dim(2).extent();
            const int filter_width = filter_buf.dim(1).extent();
            const int filter_height = filter_buf.dim(2).extent();
            const int output_width = output_buf.dim(1).extent();
            const int output_height = output_buf.dim(2).extent();

            const int dilated_filter_width = dilation_[0] * (filter_width - 1) + 1;
            const int dilated_filter_height = dilation_[1] * (filter_height - 1) + 1;

            const int pad_width = std::max(0,
                                           ((output_width - 1) * stride_[0] + dilated_filter_width - input_width) / 2);
            const int pad_height = std::max(0,
                                            ((output_height - 1) * stride_[1] + dilated_filter_height - input_height) / 2);

            input_buf.translate({0, pad_width, pad_height, 0});
        }

        if (depth_multiplier_ >= output_buf.dim(0).extent()) {
            APP_CHECK(
                0 == DepthwiseConvolutionUint8Broadcast(
                         input_buf, filter_buf, bias_buf, depth_multiplier,
                         (uint8_t)input_offset, (uint8_t)filter_offset, stride_[0], stride_[1],
                         dilation_[0], dilation_[1], output_multiplier, output_shift,
                         (uint8_t)output_offset, (uint8_t)output_range.min, (uint8_t)output_range.max, output_buf));
        } else {
            APP_CHECK(
                0 == DepthwiseConvolutionUint8(
                         input_buf, filter_buf, bias_buf, depth_multiplier,
                         (uint8_t)input_offset, (uint8_t)filter_offset, stride_[0], stride_[1],
                         dilation_[0], dilation_[1], output_multiplier, output_shift,
                         (uint8_t)output_offset, (uint8_t)output_range.min, (uint8_t)output_range.max, output_buf));
        }
    }
}

void MaxPoolOp::execute(const Box &crop) {
    const Tensor *in = input();
    Tensor *out = output();

    if (in->type() == TensorType::UInt8 &&
        out->type() == TensorType::UInt8) {
        auto input_buf = in->data<uint8_t>();
        auto output_buf = out->data<uint8_t>(crop);

        const auto output_range = GetoutRange(activation_, out);

        APP_CHECK(
            0 == MaxPoolUint8(input_buf, stride_[0], stride_[1],
                              filter_size_[0], filter_size_[1],
                              output_range.min, output_range.max, output_buf));
    }
}

Op::Bounds PadOp::infer_bounds(const Box &crop) const {
    auto padding = input(1)->data<const int32_t>();

    Bounds result;

    Box padded_crop = crop;
    for (int d = 0; d < 4; d++) {
        padded_crop[d] += padding(d);
    }

    result.inputs.emplace_back(
        intersect(padded_crop, without_strides(input(0)->shape())));
    result.inputs.emplace_back(without_strides(input(1)->shape()));
    result.outputs.emplace_back(crop);
    return result;
}

std::vector<Box> PadOp::split(const Box &crop) const {
    const int ksplit = 2;
    return split_crop(crop, 2, ksplit);
}

void PadOp::execute(const Box &crop) {
    const Tensor *in = input(0);
    auto padding = input(1)->data<const int32_t>();
    Tensor *out = output();

    if (sizeof_tensor_type(out->type()) == 1) {
        auto input_buf = in->data<uint8_t>();
        auto output_buf = out->data<uint8_t>(crop);

        uint8_t pad_value = 0;

        for (int i = 0; i < 4; i++) {
            input_buf.translate(i, padding(i));
        }

        // TODO: This is pretty hard to beat, but surely it's possible.
        for (int y = output_buf.dim(2).min(); y <= output_buf.dim(2).max(); y++) {
            auto output_y = output_buf.sliced(2, y);
            output_y.fill(pad_value);
            if (input_buf.dim(2).min() <= y && y <= input_buf.dim(2).max()) {
                auto input_y = input_buf.sliced(2, y);
                output_y.copy_from(input_y);
            }
        }
    }
}

// TODO: Maybe this is only a reshape in some dimensions, in which case we might be able to split it.
Op::Bounds ReshapeOp::infer_bounds(const Box &crop) const {
    Bounds result;
    result.inputs = {without_strides(input()->shape())};
    result.outputs = {crop};
    return result;
}

std::vector<Box> ReshapeOp::split(const Box &crop) const {
    return {crop};
}

void ReshapeOp::execute(const Box &crop) {
    const Tensor *in = input();
    Tensor *out = output();

    if (in->type() == TensorType::UInt8 &&
        out->type() == TensorType::UInt8) {
        auto input_buf = in->data<uint8_t>();
        auto output_buf = out->data<uint8_t>(crop);

        // TODO: This should probably just be implemented by aliasing two of the tensors.
        APP_CHECK(input_buf.number_of_elements() == output_buf.number_of_elements());
        APP_CHECK(in->is_allocated());
        APP_CHECK(out->is_allocated());
        // TODO: This should also check the strides are dense.
        memcpy(output_buf.data(), input_buf.data(), input_buf.number_of_elements());
    }
}

}  // namespace interpret_nn
