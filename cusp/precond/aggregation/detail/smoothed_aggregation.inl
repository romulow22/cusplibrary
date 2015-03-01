/*
 *  Copyright 2008-2014 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <cusp/elementwise.h>
#include <cusp/format_utils.h>
#include <cusp/multiply.h>

#include <cusp/blas/blas.h>

namespace cusp
{
namespace precond
{
namespace aggregation
{
namespace detail
{
template <typename Matrix>
void setup_level_matrix(Matrix& dst, Matrix& src)
{
    dst.swap(src);
}

template <typename Matrix1, typename Matrix2>
void setup_level_matrix(Matrix1& dst, Matrix2& src)
{
    dst = src;
}
} // end namespace detail

template <typename IndexType, typename ValueType, typename MemorySpace, typename SmootherType, typename SolverType>
template <typename MatrixType>
smoothed_aggregation<IndexType,ValueType,MemorySpace,SmootherType,SolverType>
::smoothed_aggregation(const MatrixType& A, const SAOptionsType& sa_options)
    : sa_options(sa_options)
{
    sa_initialize(A);
}

template <typename IndexType, typename ValueType, typename MemorySpace, typename SmootherType, typename SolverType>
template <typename MatrixType,typename ArrayType>
smoothed_aggregation<IndexType,ValueType,MemorySpace,SmootherType,SolverType>
::smoothed_aggregation(const MatrixType& A, const ArrayType& B, const SAOptionsType& sa_options)
    : sa_options(sa_options)
{
    sa_initialize(A,B);
}

template <typename IndexType, typename ValueType, typename MemorySpace, typename SmootherType, typename SolverType>
template <typename MemorySpace2, typename SmootherType2, typename SolverType2>
smoothed_aggregation<IndexType,ValueType,MemorySpace,SmootherType,SolverType>
::smoothed_aggregation(const smoothed_aggregation<IndexType,ValueType,MemorySpace2,SmootherType2,SolverType2>& M)
    : sa_options(M.sa_options), Parent(M)
{
    for( size_t lvl = 0; lvl < M.sa_levels.size(); lvl++ )
        sa_levels.push_back(M.sa_levels[lvl]);
}

template <typename IndexType, typename ValueType, typename MemorySpace, typename SmootherType, typename SolverType>
template <typename MatrixType>
void smoothed_aggregation<IndexType,ValueType,MemorySpace,SmootherType,SolverType>
::sa_initialize(const MatrixType& A)
{
    cusp::constant_array<ValueType> B(A.num_rows, 1);
    sa_initialize(A,B);
}

template <typename IndexType, typename ValueType, typename MemorySpace, typename SmootherType, typename SolverType>
template <typename MatrixType, typename ArrayType>
void smoothed_aggregation<IndexType,ValueType,MemorySpace,SmootherType,SolverType>
::sa_initialize(const MatrixType& A, const ArrayType& B)
{
    typedef typename MatrixType::coo_view_type CooView;
    typedef typename SolveMatrixType::format   Format;
    typedef cusp::detail::matrix_base<IndexType,ValueType,MemorySpace,Format> BaseMatrixType;

    if(sa_levels.size() > 0)
    {
      sa_levels.resize(0);
      Parent::levels.resize(0);
    }

    Parent::resize(A.num_rows, A.num_cols, A.num_entries);
    Parent::levels.reserve(sa_options.max_levels); // avoid reallocations which force matrix copies
    Parent::levels.push_back(typename Parent::level());

    sa_levels.push_back(sa_level<SetupMatrixType>());
    sa_levels.back().B = B;

    // Setup the first level using a COO view
    if(A.num_rows > sa_options.min_level_size)
    {
        CooView A_(A);
        extend_hierarchy(A_);

        Parent::levels.back().smoother = SmootherType(A);
    }

    // Iteratively setup lower levels until stopping criteria are reached
    while ((sa_levels.back().A_.num_rows > sa_options.min_level_size) &&
           (sa_levels.size() < sa_options.max_levels))
        extend_hierarchy(sa_levels.back().A_);

    // Initialize coarse solver
    Parent::solver = SolverType(sa_levels.back().A_);

    for( size_t lvl = 1; lvl < sa_levels.size(); lvl++ )
    {
        // Setup solve matrix for each level
        detail::setup_level_matrix( Parent::levels[lvl].A, sa_levels[lvl].A_ );

        // Initialize smoother for each level
        Parent::levels.back().smoother = SmootherType(Parent::levels[lvl].A);
    }

    // Resize first level of multilevel solver but avoid allocations
    // necessary to ensure multilevel statistics are correct
    static_cast<BaseMatrixType&>(Parent::levels[0].A).resize(A.num_rows, A.num_cols, A.num_entries);
}

template <typename IndexType, typename ValueType, typename MemorySpace, typename SmootherType, typename SolverType>
template <typename MatrixType>
void smoothed_aggregation<IndexType,ValueType,MemorySpace,SmootherType,SolverType>
::extend_hierarchy(const MatrixType& A)
{
    cusp::array1d<IndexType,MemorySpace> aggregates;
    {
        // compute stength of connection matrix
        SetupMatrixType C;
        sa_options.strength_of_connection(A, C);

        // compute aggregates
        aggregates.resize(C.num_rows);
        cusp::blas::fill(aggregates,IndexType(0));
        sa_options.aggregate(C, aggregates);
    }

    SetupMatrixType P;
    cusp::array1d<ValueType,MemorySpace>  B_coarse;
    {
        // compute tenative prolongator and coarse nullspace vector
        SetupMatrixType 				T;
        sa_options.fit_candidates(aggregates, sa_levels.back().B, T, B_coarse);

        // compute prolongation operator
        sa_options.smooth_prolongator(A, T, P, sa_levels.back().rho_DinvA);  // TODO if C != A then compute rho_Dinv_C
    }

    // compute restriction operator (transpose of prolongator)
    SetupMatrixType R;
    sa_options.form_restriction(P,R);

    // construct Galerkin product R*A*P
    SetupMatrixType RAP;
    sa_options.galerkin_product(R,A,P,RAP);

    // Setup components for next level in hierarchy
    sa_levels.back().aggregates.swap(aggregates);
    sa_levels.push_back(sa_level<SetupMatrixType>());
    sa_levels.back().A_.swap(RAP);
    sa_levels.back().B.swap(B_coarse);

    detail::setup_level_matrix( Parent::levels.back().R, R );
    detail::setup_level_matrix( Parent::levels.back().P, P );
    Parent::levels.back().residual.resize(A.num_rows);

    Parent::levels.push_back(typename Parent::level());
    Parent::levels.back().x.resize(sa_levels.back().A_.num_rows);
    Parent::levels.back().b.resize(sa_levels.back().A_.num_rows);
}

} // end namespace aggregation
} // end namespace precond
} // end namespace cusp

