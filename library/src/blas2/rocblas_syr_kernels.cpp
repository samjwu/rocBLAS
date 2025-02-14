/* ************************************************************************
 * Copyright (C) 2016-2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell cop-
 * ies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IM-
 * PLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNE-
 * CTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ************************************************************************ */

#include "check_numerics_vector.hpp"
#include "handle.hpp"
#include "rocblas.h"
#include "rocblas_syr.hpp"

template <bool UPPER, rocblas_int DIM_X, typename T, typename U, typename V, typename W>
ROCBLAS_KERNEL(DIM_X)
rocblas_syr_kernel_inc1(rocblas_int    n,
                        size_t         area,
                        U              alpha_device_host,
                        rocblas_stride stride_alpha,
                        V              xa,
                        rocblas_stride shiftx,
                        rocblas_stride stridex,
                        W              Aa,
                        rocblas_stride shiftA,
                        rocblas_int    lda,
                        rocblas_stride strideA)
{
    auto alpha = load_scalar(alpha_device_host, blockIdx.z, stride_alpha);
    if(!alpha)
        return;

    const auto* __restrict__ x = load_ptr_batch(xa, blockIdx.z, shiftx, stridex);
    T* __restrict__ A          = load_ptr_batch(Aa, blockIdx.z, shiftA, strideA);

    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x; // linear area index
    if(i >= area)
        return;

    size_t ri = !UPPER ? area - 1 - i : i;

    // linearized triangle with diagonal to col, row
    int         k  = (int)((sqrt(8 * ri + 1) - 1) / 2);
    rocblas_int ty = k;
    rocblas_int tx = ri - k * (k + 1) / 2;

    if(!UPPER)
    {
        int maxIdx = n - 1;
        tx         = maxIdx - tx;
        ty         = maxIdx - ty;
    }

    A[tx + size_t(lda) * ty] += alpha * x[tx] * x[ty];

    // original algorithm run over rectangular space
    // if(uplo == rocblas_fill_lower ? tx < n && ty <= tx : ty < n && tx <= ty)
    // A[tx + size_t(lda) * ty] += alpha * x[tx] * x[ty];
}

template <bool UPPER, rocblas_int DIM_X, typename T, typename U, typename V, typename W>
ROCBLAS_KERNEL(DIM_X)
rocblas_syr_kernel(rocblas_int    n,
                   size_t         area,
                   U              alpha_device_host,
                   rocblas_stride stride_alpha,
                   V              xa,
                   rocblas_stride shiftx,
                   rocblas_int    incx,
                   rocblas_stride stridex,
                   W              Aa,
                   rocblas_stride shiftA,
                   rocblas_int    lda,
                   rocblas_stride strideA)
{
    auto alpha = load_scalar(alpha_device_host, blockIdx.z, stride_alpha);
    if(!alpha)
        return;

    const auto* __restrict__ x = load_ptr_batch(xa, blockIdx.z, shiftx, stridex);
    T* __restrict__ A          = load_ptr_batch(Aa, blockIdx.z, shiftA, strideA);

    size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x; // linear area index
    if(i >= area)
        return;

    size_t ri = !UPPER ? area - 1 - i : i;

    // linearized triangle with diagonal to col, row
    int         k  = (int)((sqrt(8 * ri + 1) - 1) / 2);
    rocblas_int ty = k;
    rocblas_int tx = ri - k * (k + 1) / 2;

    if(!UPPER)
    {
        int maxIdx = n - 1;
        tx         = maxIdx - tx;
        ty         = maxIdx - ty;
    }

    A[tx + size_t(lda) * ty] += alpha * x[tx * int64_t(incx)] * x[ty * int64_t(incx)];
}

template <typename T, typename U, typename V, typename W>
ROCBLAS_INTERNAL_EXPORT_NOINLINE rocblas_status
    rocblas_internal_syr_template(rocblas_handle handle,
                                  rocblas_fill   uplo,
                                  rocblas_int    n,
                                  U              alpha,
                                  rocblas_stride stride_alpha,
                                  V              x,
                                  rocblas_stride offsetx,
                                  rocblas_int    incx,
                                  rocblas_stride stridex,
                                  W              A,
                                  rocblas_stride offseta,
                                  rocblas_int    lda,
                                  rocblas_stride strideA,
                                  rocblas_int    batch_count)
{
    // Quick return
    if(!n || batch_count == 0)
        return rocblas_status_success;

    hipStream_t rocblas_stream = handle->get_stream();

    static constexpr int SYR_DIM_X = 1024;

    size_t nitems = (size_t)n * (n + 1) / 2;

    rocblas_int blocksX = (nitems - 1) / (SYR_DIM_X) + 1;

    dim3 syr_grid(blocksX, 1, batch_count);
    dim3 syr_threads(SYR_DIM_X);

    // in case of negative inc shift pointer to end of data for negative indexing tid*inc
    auto shiftx = incx < 0 ? offsetx - ptrdiff_t(incx) * (n - 1) : offsetx;

    if(rocblas_pointer_mode_device == handle->pointer_mode)
    {
        if(uplo == rocblas_fill_upper)
        {
            if(incx == 1)
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel_inc1<true, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
            else
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel<true, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      incx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
        }
        else
        {
            if(incx == 1)
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel_inc1<false, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
            else
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel<false, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      incx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
        }
    }
    else // host pointer mode
    {
        if(uplo == rocblas_fill_upper)
        {
            if(incx == 1)
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel_inc1<true, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      *alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
            else
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel<true, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      *alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      incx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
        }
        else
        {
            if(incx == 1)
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel_inc1<false, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      *alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
            else
                ROCBLAS_LAUNCH_KERNEL((rocblas_syr_kernel<false, SYR_DIM_X, T>),
                                      syr_grid,
                                      syr_threads,
                                      0,
                                      rocblas_stream,
                                      n,
                                      nitems,
                                      *alpha,
                                      stride_alpha,
                                      x,
                                      shiftx,
                                      incx,
                                      stridex,
                                      A,
                                      offseta,
                                      lda,
                                      strideA);
        }
    }
    return rocblas_status_success;
}

template <typename T, typename U>
rocblas_status rocblas_syr_check_numerics(const char*    function_name,
                                          rocblas_handle handle,
                                          rocblas_fill   uplo,
                                          rocblas_int    n,
                                          T              A,
                                          rocblas_stride offset_a,
                                          rocblas_int    lda,
                                          rocblas_stride stride_a,
                                          U              x,
                                          rocblas_stride offset_x,
                                          rocblas_int    inc_x,
                                          rocblas_stride stride_x,
                                          rocblas_int    batch_count,
                                          const int      check_numerics,
                                          bool           is_input)
{
    rocblas_status check_numerics_status
        = rocblas_internal_check_numerics_matrix_template(function_name,
                                                          handle,
                                                          rocblas_operation_none,
                                                          uplo,
                                                          rocblas_client_symmetric_matrix,
                                                          n,
                                                          n,
                                                          A,
                                                          offset_a,
                                                          lda,
                                                          stride_a,
                                                          batch_count,
                                                          check_numerics,
                                                          is_input);

    if(check_numerics_status != rocblas_status_success)
        return check_numerics_status;

    if(is_input)
    {
        check_numerics_status = rocblas_internal_check_numerics_vector_template(function_name,
                                                                                handle,
                                                                                n,
                                                                                x,
                                                                                offset_x,
                                                                                inc_x,
                                                                                stride_x,
                                                                                batch_count,
                                                                                check_numerics,
                                                                                is_input);
    }
    return check_numerics_status;
}

// Instantiations below will need to be manually updated to match any change in
// template parameters in the files *syr*.cpp

// clang-format off

#ifdef INSTANTIATE_SYR_TEMPLATE
#error INSTANTIATE_SYR_TEMPLATE already defined
#endif

#define INSTANTIATE_SYR_TEMPLATE(T_, U_, V_, W_)                                       \
template ROCBLAS_INTERNAL_EXPORT_NOINLINE rocblas_status rocblas_internal_syr_template \
                                 <T_, U_, V_, W_>                                      \
                                 (rocblas_handle handle,                               \
                                  rocblas_fill   uplo,                                 \
                                  rocblas_int    n,                                    \
                                  U_              alpha,                               \
                                  rocblas_stride stride_alpha,                         \
                                  V_              x,                                   \
                                  rocblas_stride    offsetx,                              \
                                  rocblas_int    incx,                                 \
                                  rocblas_stride stridex,                              \
                                  W_              A,                                   \
                                  rocblas_stride    offseta,                              \
                                  rocblas_int    lda,                                  \
                                  rocblas_stride strideA,                              \
                                  rocblas_int    batch_count);

INSTANTIATE_SYR_TEMPLATE(float, float const*, float const*, float*);
INSTANTIATE_SYR_TEMPLATE(double, double const*, double const*, double*);
INSTANTIATE_SYR_TEMPLATE(rocblas_float_complex, rocblas_float_complex const*, rocblas_float_complex const*, rocblas_float_complex*);
INSTANTIATE_SYR_TEMPLATE(rocblas_double_complex, rocblas_double_complex const*, rocblas_double_complex const*, rocblas_double_complex*);
INSTANTIATE_SYR_TEMPLATE(float, float const*, float const* const*, float* const*);
INSTANTIATE_SYR_TEMPLATE(double, double const*, double const* const*, double* const*);
INSTANTIATE_SYR_TEMPLATE(rocblas_float_complex, rocblas_float_complex const*, rocblas_float_complex const* const*, rocblas_float_complex* const*);
INSTANTIATE_SYR_TEMPLATE(rocblas_double_complex, rocblas_double_complex const*, rocblas_double_complex const* const*, rocblas_double_complex* const*);

#undef INSTANTIATE_SYR_TEMPLATE

#ifdef INSTANTIATE_SYR_NUMERICS
#error INSTANTIATE_SYR_NUMERICS already defined
#endif

#define INSTANTIATE_SYR_NUMERICS(T_, U_)                                 \
template rocblas_status rocblas_syr_check_numerics                       \
                                         <T_, U_>                        \
                                         (const char*    function_name,  \
                                          rocblas_handle handle,         \
                                          rocblas_fill   uplo,           \
                                          rocblas_int    n,              \
                                          T_              A,             \
                                          rocblas_stride    offset_a,       \
                                          rocblas_int    lda,            \
                                          rocblas_stride stride_a,       \
                                          U_              x,             \
                                          rocblas_stride    offset_x,       \
                                          rocblas_int    inc_x,          \
                                          rocblas_stride stride_x,       \
                                          rocblas_int    batch_count,    \
                                          const int      check_numerics, \
                                          bool           is_input);

INSTANTIATE_SYR_NUMERICS(float*, float const*);
INSTANTIATE_SYR_NUMERICS(double*, double const*);
INSTANTIATE_SYR_NUMERICS(rocblas_float_complex*, rocblas_float_complex const*);
INSTANTIATE_SYR_NUMERICS(rocblas_double_complex*, rocblas_double_complex const*);
INSTANTIATE_SYR_NUMERICS(float* const*, float const* const*);
INSTANTIATE_SYR_NUMERICS(double* const*, double const* const*);
INSTANTIATE_SYR_NUMERICS(rocblas_float_complex* const*, rocblas_float_complex const* const*);
INSTANTIATE_SYR_NUMERICS(rocblas_double_complex* const*, rocblas_double_complex const* const*);

#undef INSTANTIATE_SYR_NUMERICS

// clang-format on
