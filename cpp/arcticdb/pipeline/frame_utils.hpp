/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <arcticdb/pipeline/pipeline_context.hpp>
#include <arcticdb/column_store/string_pool.hpp>
#include <arcticdb/column_store/chunked_buffer.hpp>
#include <arcticdb/pipeline/frame_slice.hpp>
#include <arcticdb/entity/atom_key.hpp>
#include <arcticdb/pipeline/input_tensor_frame.hpp>
#include <arcticdb/stream/protobuf_mappings.hpp>
#include <arcticdb/entity/protobuf_mappings.hpp>
#include <arcticdb/python/gil_lock.hpp>
#include <arcticdb/python/python_types.hpp>
#include <arcticdb/python/python_to_tensor_frame.hpp>
#include <arcticdb/pipeline/string_pool_utils.hpp>
#include <util/flatten_utils.hpp>
#include <arcticdb/entity/timeseries_descriptor.hpp>

namespace arcticdb {

inline size_t get_first_string_size(const pipelines::PipelineContextRow& context_row, ChunkedBuffer &src, std::size_t first_row_in_frame) {
    auto offset = first_context_row(context_row.slice_and_key(), first_row_in_frame);
    auto num_rows = context_row.slice_and_key().slice_.row_range.diff();
    util::check(context_row.has_string_pool(), "String pool not found for context row {}", context_row.index());
    return get_first_string_size(num_rows, src, offset, context_row.string_pool());
}

inline size_t get_max_string_size(const pipelines::PipelineContextRow& context_row, ChunkedBuffer &src, std::size_t first_row_in_frame) {
    auto offset = first_context_row(context_row.slice_and_key(), first_row_in_frame);
    auto num_rows = context_row.slice_and_key().slice_.row_range.diff();
    size_t max_length{0u};

    for(auto row = 0u; row < num_rows; ++row) {
        auto offset_val = get_offset_string_at(offset + row, src);
        if (offset_val == nan_placeholder() || offset_val == not_a_string())
            continue;

        max_length = std::max(max_length, get_string_from_pool(offset_val, context_row.string_pool()).size());
    }
    return max_length;
}

TimeseriesDescriptor make_timeseries_descriptor(
    size_t total_rows,
    StreamDescriptor&& desc,
    arcticdb::proto::descriptors::NormalizationMetadata&& norm_meta,
    std::optional<arcticdb::proto::descriptors::UserDefinedMetadata>&& um,
    std::optional<AtomKey>&& prev_key,
    std::optional<AtomKey>&& next_key,
    bool bucketize_dynamic
    );

TimeseriesDescriptor timseries_descriptor_from_index_segment(
    size_t total_rows,
    pipelines::index::IndexSegmentReader&& index_segment_reader,
    std::optional<AtomKey>&& prev_key,
    bool bucketize_dynamic
    );

TimeseriesDescriptor timeseries_descriptor_from_pipeline_context(
    const std::shared_ptr<pipelines::PipelineContext>& pipeline_context,
    std::optional<AtomKey>&& prev_key,
    bool bucketize_dynamic);


TimeseriesDescriptor index_descriptor_from_frame(
    pipelines::InputTensorFrame&& frame,
    size_t existing_rows,
    std::optional<entity::AtomKey>&& prev_key = {});

template <typename RawType>
RawType* flatten_tensor(
        std::optional<ChunkedBuffer>& flattened_buffer,
        size_t rows_to_write,
        const NativeTensor& tensor,
        size_t slice_num,
        size_t regular_slice_size
        ) {
    flattened_buffer = ChunkedBuffer::presized(rows_to_write * sizeof(RawType));
    TypedTensor<RawType> t(tensor, slice_num, regular_slice_size, rows_to_write);
    util::FlattenHelper flattener{t};
    auto dst = reinterpret_cast<RawType*>(flattened_buffer->data());
    flattener.flatten(dst, reinterpret_cast<RawType const*>(t.data()));
    return reinterpret_cast<RawType*>(flattened_buffer->data());
}

template<typename Tensor, typename Aggregator>
std::optional<convert::StringEncodingError> aggregator_set_data(
    const TypeDescriptor &type_desc,
    Tensor &tensor,
    Aggregator &agg,
    size_t col,
    size_t rows_to_write,
    size_t row,
    size_t slice_num,
    size_t regular_slice_size,
    bool sparsify_floats
) {
    return type_desc.visit_tag([&](auto &&tag) {
        using RawType = typename std::decay_t<decltype(tag)>::DataTypeTag::raw_type;
        constexpr auto dt = std::decay_t<decltype(tag)>::DataTypeTag::data_type;

        util::check(type_desc.data_type() == tensor.data_type(), "Type desc {} != {} tensor type", type_desc.data_type(),
                    tensor.data_type());
        util::check(type_desc.data_type() == dt, "Type desc {} != {} static type", type_desc.data_type(), dt);
        auto c_style = util::is_cstyle_array<RawType>(tensor);
        std::optional<ChunkedBuffer> flattened_buffer;
        if constexpr (is_sequence_type(dt)) {
            ARCTICDB_SUBSAMPLE_AGG(SetDataString)
            if (is_fixed_string_type(dt)) {
                // deduplicate the strings
                auto str_stride = tensor.strides(0);
                auto data = const_cast<void *>(tensor.data());
                auto char_data = reinterpret_cast<char *>(data) + row * str_stride;
                auto str_len = tensor.elsize();

                for (size_t s = 0; s < rows_to_write; ++s, char_data += str_stride) {
                    agg.set_string_at(col, s, char_data, str_len);
                }
            } else {
                auto data = const_cast<void *>(tensor.data());
                auto ptr_data = reinterpret_cast<PyObject **>(data);
                ptr_data += row;
                if (!c_style)
                    ptr_data = flatten_tensor<PyObject*>(flattened_buffer, rows_to_write, tensor, slice_num, regular_slice_size);

                auto none = py::none{};
                std::variant<convert::StringEncodingError, convert::PyStringWrapper> wrapper_or_error;
                // GIL will be acquired if there is a string that is not pure ASCII/UTF-8
                // In this case a PyObject will be allocated by convert::py_unicode_to_buffer
                // If such a string is encountered in a column, then the GIL will be held until that whole column has
                // been processed, on the assumption that if a column has one such string it will probably have many.
                std::optional<ScopedGILLock> scoped_gil_lock;
                auto& column = agg.segment().column(col);
                column.allocate_data(rows_to_write * sizeof(StringPool::offset_t));
                auto out_ptr = reinterpret_cast<StringPool::offset_t*>(column.buffer().data());
                auto& string_pool = agg.segment().string_pool();
                for (size_t s = 0; s < rows_to_write; ++s, ++ptr_data) {
                    if (*ptr_data == none.ptr()) {
                        *out_ptr++ = not_a_string();
                    } else if(is_py_nan(*ptr_data)){
                        *out_ptr++ = nan_placeholder();
                    } else {
                        if constexpr (is_utf_type(slice_value_type(dt))) {
                            wrapper_or_error = convert::py_unicode_to_buffer(*ptr_data, scoped_gil_lock);
                        } else {
                            wrapper_or_error = convert::pystring_to_buffer(*ptr_data, false);
                        }
                        // Cannot use util::variant_match as only one of the branches would have a return type
                        if (std::holds_alternative<convert::PyStringWrapper>(wrapper_or_error)) {
                            convert::PyStringWrapper wrapper(std::move(std::get<convert::PyStringWrapper>(wrapper_or_error)));
                            const auto offset = string_pool.get(wrapper.buffer_, wrapper.length_);
                            *out_ptr++ = offset.offset();
                        } else if (std::holds_alternative<convert::StringEncodingError>(wrapper_or_error)) {
                            auto error = std::get<convert::StringEncodingError>(wrapper_or_error);
                            error.row_index_in_slice_ = s;
                            return std::optional<convert::StringEncodingError>(error);
                        } else {
                            internal::raise<ErrorCode::E_ASSERTION_FAILURE>("Unexpected variant alternative");
                        }
                    }
                }
            }
        } else if constexpr (is_numeric_type(dt) || is_bool_type(dt)) {
            auto ptr = tensor.template ptr_cast<RawType>(row);
            if (sparsify_floats) {
                if constexpr (is_floating_point_type(dt)) {
                    agg.set_sparse_block(col, ptr, rows_to_write);
                } else {
                    util::raise_rte("sparse currently supported for floating point columns only.");
                }
            } else {
                if (c_style) {
                    ARCTICDB_SUBSAMPLE_AGG(SetDataZeroCopy)
                    agg.set_external_block(col, ptr, rows_to_write);
                } else {
                    ARCTICDB_SUBSAMPLE_AGG(SetDataFlatten)
                    ARCTICDB_DEBUG(log::version(),
                            "Data contains non-contiguous columns, writing will be inefficient, consider coercing to c_style ndarray (shape={}, data_size={})",
                            tensor.strides(0),
                            sizeof(RawType));

                    TypedTensor <RawType> t(tensor, slice_num, regular_slice_size, rows_to_write);
                    agg.set_array(col, t);
                }
            }
        }  else if constexpr (!is_empty_type(dt)) {
            static_assert(!sizeof(dt), "Unknown data type");
        }
        return std::optional<convert::StringEncodingError>();
    });
}

namespace pipelines {
struct SliceAndKey;
struct PipelineContext;
}

size_t adjust_slice_rowcounts(
    std::vector<pipelines::SliceAndKey> & slice_and_keys);

void adjust_slice_rowcounts(
    const std::shared_ptr<pipelines::PipelineContext>& pipeline_context);

size_t get_slice_rowcounts(
    std::vector<pipelines::SliceAndKey> & slice_and_keys);

std::pair<size_t, size_t> offset_and_row_count(
    const std::shared_ptr<pipelines::PipelineContext>& context);

} //namespace arcticdb
