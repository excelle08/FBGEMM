/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifdef __linux__

#define FBGEMM_EXPORTS
#include "./EmbeddingSpMDMAutovec.h"

#include "fbgemm/FbgemmBuild.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>

using std::vector;

namespace fbgemm {

template <typename IndexType, typename OffsetType, typename OutType>
bool EmbeddingSpMDM8Bit_autovec(
    const int64_t block_size,
    const int64_t output_size,
    const int64_t index_size,
    const int64_t data_size,
    const uint8_t* input,
    const IndexType* indices,
    const OffsetType* offsets_or_lengths,
    const float* weights, // optional, can be null for non-weighted sum
    bool normalize_by_lengths,
    OutType* out,
    bool is_weight_positional /*=false*/,
    bool use_offsets /*=true*/,
    int64_t output_stride /*=-1*/,
    int64_t input_stride /*=-1*/,
    bool scale_bias_last /*=true*/,
    bool no_bag /*=false*/,
    bool is_bf16_out /*=false*/) {
  const bool isOutput8bit = std::is_same<OutType, uint8_t>::value;
  if (output_stride == -1) {
    output_stride = block_size;
  }
  if constexpr (isOutput8bit) {
    assert(input_stride == output_stride);
  }
  vector<float> buf(block_size);

  // block_size is the number of elements and fused_block_size is the size of
  // an entire row, including scale and bias.
  if (input_stride == -1) {
    // scale_bias_last == false is for table batched embedding that stores
    // scale and bias in float16
    const auto scale_bias_offset =
        2 * (scale_bias_last ? sizeof(float) : sizeof(float16));
    input_stride = block_size + scale_bias_offset;
  }
  constexpr int64_t max_initial_prefetch_rows = 4;
  const int64_t prefetch_stride =
      std::min(max_initial_prefetch_rows, index_size);
  for (int pf_idx = 0; pf_idx < prefetch_stride; ++pf_idx) {
    do_prefetch(
        reinterpret_cast<const char*>(input + input_stride * indices[pf_idx]),
        0,
        0);
  }
  int64_t current = 0;

  if (no_bag) {
    for (int m = 0; m < output_size; ++m) {
      int64_t idx = indices[m];

      if (idx < 0 || idx >= data_size) {
        return false;
      }
      if constexpr (isOutput8bit) {
        const uint8_t* input_row_ptr = input + input_stride * idx;
        memcpy(out, input_row_ptr, sizeof(uint8_t) * input_stride);
      } else {
        memset(buf.data(), 0, sizeof(float) * block_size);
        const float* scale_bias = reinterpret_cast<const float*>(
            input + input_stride * idx + (scale_bias_last ? block_size : 0));

        float weight = 1.0f;
        if (weights) {
          weight = weights[m];
        }

        float scale, bias;
        if (scale_bias_last) {
          scale = weight * scale_bias[0];
          bias = weight * scale_bias[1];
        } else {
          scale = weight *
              cpu_half2float(reinterpret_cast<const float16*>(scale_bias)[0]);
          bias = weight *
              cpu_half2float(reinterpret_cast<const float16*>(scale_bias)[1]);
        }

        size_t input_offset =
            input_stride * idx + (scale_bias_last ? 0 : 2 * sizeof(float16));
#pragma omp simd
        for (int j = 0; j < block_size; ++j) {
          buf[j] =
              std::fma(scale, (float)input[input_offset + j], buf[j] + bias);
        }
        if (std::is_same<OutType, float>::value) {
#pragma omp simd
          for (int j = 0; j < block_size; ++j) {
            out[j] = buf[j];
          }
        } else if (std::is_same<OutType, uint16_t>::value && is_bf16_out) {
#pragma omp simd
          for (int j = 0; j < block_size; ++j) {
            out[j] = cpu_bf162float(buf[j]);
          }
        } else {
#pragma omp simd
          for (int j = 0; j < block_size; ++j) {
            out[j] = cpu_half2float(buf[j]);
          }
        }
        out += output_stride;
      }
    } // m
    return true;
  } // no_bag

  for (int m = 0; m < output_size; ++m) {
    memset(buf.data(), 0, sizeof(float) * block_size);
    int len = use_offsets ? offsets_or_lengths[m + 1] - offsets_or_lengths[m]
                          : offsets_or_lengths[m];
    if (current + len > index_size) {
      return false;
    }
    for (int i = 0; i < len; ++i) {
      int64_t idx = indices[current];
      int64_t prefetch_idx =
          indices[std::min(current + prefetch_stride, index_size - 1)];
      for (int64_t col = 0; col < input_stride; col += 64) {
        do_prefetch(
            reinterpret_cast<const char*>(
                input + input_stride * prefetch_idx + col),
            1);
      }
      if (!scale_bias_last && idx == -1) {
        // When scale_bias_last == false, assume this is for table batched
        // embedding (TBE) that can get -1 for pruned rows.
        continue;
      }
      if (idx < 0 || idx >= data_size) {
        return false;
      }

      const float* scale_bias = reinterpret_cast<const float*>(
          input + input_stride * idx + (scale_bias_last ? block_size : 0));

      float weight = 1.0f;
      if (weights) {
        weight = weights[is_weight_positional ? i : current];
      }
      float scale, bias;
      if (scale_bias_last) {
        scale = weight * scale_bias[0];
        bias = weight * scale_bias[1];
      } else {
        scale = weight *
            cpu_half2float(reinterpret_cast<const float16*>(scale_bias)[0]);
        bias = weight *
            cpu_half2float(reinterpret_cast<const float16*>(scale_bias)[1]);
      }

      size_t input_offset =
          input_stride * idx + (scale_bias_last ? 0 : 2 * sizeof(float16));
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        buf[j] = std::fma(scale, (float)input[input_offset + j], buf[j] + bias);
      }

      ++current;
    }
    if (normalize_by_lengths && len) {
      float scale = 1.f / len;
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        buf[j] *= scale;
      }
    }
    if (std::is_same<OutType, float>::value) {
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        out[j] = buf[j];
      }
    } else if (std::is_same<OutType, uint16_t>::value && is_bf16_out) {
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        out[j] = cpu_bf162float(buf[j]);
      }
    } else {
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        out[j] = cpu_half2float(buf[j]);
      }
    }

    out += output_stride;
  }
  return current == index_size;
}

#define INSTANTIATE_SPMDM_BASE(INDEX_TYPE, OFFSET_TYPE, OUT_TYPE) \
  template FBGEMM_API bool EmbeddingSpMDM8Bit_autovec(            \
      const int64_t block_size,                                   \
      const int64_t output_size,                                  \
      const int64_t index_size,                                   \
      const int64_t data_size,                                    \
      const uint8_t* input,                                       \
      const INDEX_TYPE* indices,                                  \
      const OFFSET_TYPE* offsets_or_lengths,                      \
      const float* weights,                                       \
      bool normalize_by_lengths,                                  \
      OUT_TYPE* out,                                              \
      bool is_weight_positional,                                  \
      bool use_offsets,                                           \
      int64_t input_stride,                                       \
      int64_t output_stride,                                      \
      bool scale_bias_last,                                       \
      bool no_bag,                                                \
      bool is_bf16_out);

#define INSTANTIATE_SPMDM_OUT_T(INDEX_TYPE, OFFSET_TYPE)   \
  INSTANTIATE_SPMDM_BASE(INDEX_TYPE, OFFSET_TYPE, float)   \
  INSTANTIATE_SPMDM_BASE(INDEX_TYPE, OFFSET_TYPE, float16) \
  INSTANTIATE_SPMDM_BASE(INDEX_TYPE, OFFSET_TYPE, std::uint8_t)

#define INSTANTIATE_SPMDM_OFFSET_T(INDEX_TYPE)      \
  INSTANTIATE_SPMDM_OUT_T(INDEX_TYPE, std::int32_t) \
  INSTANTIATE_SPMDM_OUT_T(INDEX_TYPE, std::int64_t)

#define INSTANTIATE_SPMDM_INDEX_T()        \
  INSTANTIATE_SPMDM_OFFSET_T(std::int32_t) \
  INSTANTIATE_SPMDM_OFFSET_T(std::int64_t)

INSTANTIATE_SPMDM_INDEX_T()

#undef INSTANTIATE_SPMDM_INDEX_T
#undef INSTANTIATE_SPMDM_OFFSET_T
#undef INSTANTIATE_SPMDM_OUT_T
#undef INSTANTIATE_SPMDM_BASE

template <typename IndexType, typename OffsetType, typename OutType>
bool EmbeddingSpMDMNBit_autovec(
    const int bit_rate,
    const int64_t block_size,
    const int64_t output_size,
    const int64_t index_size,
    const int64_t data_size,
    const uint8_t* input,
    const IndexType* indices,
    const OffsetType* offsets_or_lengths,
    const float* weights, // optional, can be null for non-weighted sum
    const bool normalize_by_lengths,
    OutType* out,
    const bool is_weight_positional /*=false*/,
    const bool use_offsets /*=true*/,
    int64_t output_stride /*=-1*/,
    int64_t input_stride /*=-1*/,
    const bool scale_bias_last /*=true*/,
    const bool is_bf16_out /*=false*/) {
  assert((bit_rate == 2 || bit_rate == 4) && "bit_rate must be 2 or 4");
  const int num_elem_per_byte = 8 / bit_rate;

  if (output_stride == -1) {
    output_stride = block_size;
  }

  // block_size is the number of elements and fused_block_size is the size of
  // an entire row, including scale and bias.
  const auto scale_bias_offset = 2 * sizeof(float16);
  if (input_stride == -1) {
    input_stride = div_up(block_size, num_elem_per_byte) + scale_bias_offset;
  }
  int64_t current = 0;
  const int64_t rounded_bs = round_up(block_size, num_elem_per_byte);
  vector<float> buf(rounded_bs);
  for (int m = 0; m < output_size; ++m) {
    memset(buf.data(), 0, sizeof(float) * rounded_bs);
    int len = use_offsets ? offsets_or_lengths[m + 1] - offsets_or_lengths[m]
                          : offsets_or_lengths[m];
    if (current + len > index_size) {
      return false;
    }
    constexpr int tile_size = 4;
#if _OPENMP >= 202011
#pragma omp tile sizes(tile_size)
#endif
    for (int i = 0; i < len; ++i) {
      int64_t idx = indices[current];
      int64_t prefetch_idx =
          indices[std::min(current + tile_size, index_size - 1)];
      do_prefetch(
          reinterpret_cast<const char*>(input + input_stride * prefetch_idx),
          1);
      if (idx < 0 || idx >= data_size) {
        return false;
      }

      const float16* scale_bias = reinterpret_cast<const float16*>(
          input + input_stride * idx +
          (scale_bias_last ? div_up(block_size, num_elem_per_byte) : 0));

      float scale = cpu_half2float(scale_bias[0]);
      float bias = cpu_half2float(scale_bias[1]);
      if (weights) {
        float weight = weights[is_weight_positional ? i : current];
        scale *= weight;
        bias *= weight;
      }

      const int64_t offset =
          input_stride * idx + (scale_bias_last ? 0 : scale_bias_offset);
      if (bit_rate == 4) {
        const size_t halfbufsz = (block_size + 1) / 2;
        assert(halfbufsz > 0);
        for (size_t j = 0; j < halfbufsz; ++j) {
          uint8_t tmp = input[offset + j];
          float quantized1 = float(tmp & 0xf);
          float quantized2 = float(tmp >> 4);
          buf[j * 2] = std::fma(scale, quantized1, buf[j * 2] + bias);
          buf[j * 2 + 1] = std::fma(scale, quantized2, buf[j * 2 + 1] + bias);
        }
      } else if (bit_rate == 2) {
        size_t qbufsz = (block_size + 3) / 4;
        const uint8_t mask1 = 0x3;
        const uint8_t mask2 = 0xC;
        const uint8_t mask3 = 0x30;
        for (size_t j = 0; j < qbufsz; ++j) {
          uint8_t tmp = input[offset + j];
          float quantized1 = float(tmp & mask1);
          buf[j * 4] = std::fma(scale, quantized1, buf[j * 4] + bias);
          float quantized2 = float((tmp & mask2) >> 2);
          buf[j * 4 + 1] = std::fma(scale, quantized2, buf[j * 4 + 1] + bias);
          float quantized3 = float((tmp & mask3) >> 4);
          buf[j * 4 + 2] = std::fma(scale, quantized3, buf[j * 4 + 2] + bias);
          float quantized4 = float(tmp >> 6);
          buf[j * 4 + 3] = std::fma(scale, quantized4, buf[j * 4 + 3] + bias);
        }
      }
      ++current;
    }

    if (normalize_by_lengths && len) {
      float scale = 1.f / len;
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        buf[j] *= scale;
      }
    }
    if (std::is_same<OutType, float>::value) {
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        out[j] = buf[j];
      }
    } else if (std::is_same<OutType, uint16_t>::value && is_bf16_out) {
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        out[j] = cpu_bf162float(buf[j]);
      }
    } else {
#pragma omp simd
      for (int j = 0; j < block_size; ++j) {
        out[j] = cpu_half2float(buf[j]);
      }
    }
    out += output_stride;
  }
  return current == index_size;
}

#define INSTANTIATE_SPMDM_BASE(INDEX_TYPE, OFFSET_TYPE, OUT_TYPE) \
  template FBGEMM_API bool EmbeddingSpMDMNBit_autovec(            \
      const int bit_rate,                                         \
      const int64_t block_size,                                   \
      const int64_t output_size,                                  \
      const int64_t index_size,                                   \
      const int64_t data_size,                                    \
      const uint8_t* input,                                       \
      const INDEX_TYPE* indices,                                  \
      const OFFSET_TYPE* offsets_or_lengths,                      \
      const float* weights,                                       \
      const bool normalize_by_lengths,                            \
      OUT_TYPE* out,                                              \
      const bool is_weight_positional,                            \
      const bool use_offsets,                                     \
      int64_t output_stride,                                      \
      int64_t input_stride,                                       \
      const bool scale_bias_last,                                 \
      const bool is_bf16_out);

#define INSTANTIATE_SPMDM_OUT_T(INDEX_TYPE, OFFSET_TYPE) \
  INSTANTIATE_SPMDM_BASE(INDEX_TYPE, OFFSET_TYPE, float) \
  INSTANTIATE_SPMDM_BASE(INDEX_TYPE, OFFSET_TYPE, float16)

#define INSTANTIATE_SPMDM_OFFSET_T(INDEX_TYPE) \
  INSTANTIATE_SPMDM_OUT_T(INDEX_TYPE, int32_t) \
  INSTANTIATE_SPMDM_OUT_T(INDEX_TYPE, int64_t)

INSTANTIATE_SPMDM_OFFSET_T(int32_t)
INSTANTIATE_SPMDM_OFFSET_T(int64_t)

#undef INSTANTIATE_SPMDM_OFFSET_T
#undef INSTANTIATE_SPMDM_OUT_T
#undef INSTANTIATE_SPMDM_BASE

} // namespace fbgemm

#endif // #ifdef __linux__
