/*******************************************************************************
 * Copyright (c) 2015-2018 Skymind, Inc.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License, Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/

//
//  @author raver119@gmail.com
//  @author Yurii Shyrma (iuriish@yahoo.com)
//

#include <types/types.h>
#include <op_boilerplate.h>
#include <loops/reduce_long.h>
#include <loops/legacy_ops.h>
#include <OmpLaunchHelper.h>

using namespace simdOps;

namespace functions {
    namespace reduce {
        template <typename X, typename Z>
        template <typename OpType>
        void _CUDA_H ReduceLongFunction<X,Z>::execScalar(void *vx,
                                                Nd4jLong *xShapeInfo,
                                                void *vextraParams,
                                                void *vz,
                                                Nd4jLong *zShapeInfo) {
            auto x = reinterpret_cast<X *>(vx);
            auto z = reinterpret_cast<Z *>(vz);
            auto extraParams = reinterpret_cast<X *>(vextraParams);

            const Nd4jLong length = shape::length(xShapeInfo);
            auto xEws = shape::elementWiseStride(xShapeInfo);

            if (xEws >= 1) {
                z[0] = execScalar<OpType>(x, xEws, length, extraParams);
            }
            else {

                X start = OpType::startingValue(x);

                for(Nd4jLong i = 0; i < length; ++i)                     
                    start = OpType::update(start, OpType::op(x[shape::getIndexOffset(i, xShapeInfo, length)], extraParams), extraParams);                

                z[0] = OpType::postProcess(start, shape::length(xShapeInfo), extraParams);
            }
        }


        template <typename X, typename Z>
        template <typename OpType>
            Z _CUDA_H ReduceLongFunction<X, Z>::execScalar(void *vx,
                    Nd4jLong *xShapeInfo,
                    void *vextraParams) {
                auto x = reinterpret_cast<X *>(vx);
                auto extraParams = reinterpret_cast<X *>(vextraParams);

                const Nd4jLong length = shape::length(xShapeInfo);
                auto xEws = shape::elementWiseStride(xShapeInfo);

                if (xEws >= 1) {
                    return execScalar<OpType>(x, xEws, length, extraParams);
                }
                else {

                    X start = OpType::startingValue(x);

                    for(Nd4jLong i = 0; i < length; ++i)                     
                        start = OpType::update(start, OpType::op(x[shape::getIndexOffset(i, xShapeInfo, length)], extraParams), extraParams);                                                    
                    
                    return OpType::postProcess(start, shape::length(xShapeInfo), extraParams);
                }   
            }


        template <typename X, typename Y>
        Y ReduceLongFunction<X, Y>::execScalar(const int opNum,
                void *x,
                Nd4jLong *xShapeInfo,
                void *extraParams) {
                RETURNING_DISPATCH_BY_OPNUM_TT(execScalar, PARAMS(x, xShapeInfo, extraParams), REDUCE_LONG_OPS);
        }

        template <typename X, typename Y>
        void ReduceLongFunction<X, Y>::execScalar(const int opNum,
                                        void *x,
                                        Nd4jLong *xShapeInfo,
                                        void *extraParams,
                                        void *z,
                                        Nd4jLong *zShapeInfo) {
            DISPATCH_BY_OPNUM_TT(execScalar, PARAMS(x, xShapeInfo, extraParams, z, zShapeInfo), REDUCE_LONG_OPS);
        }

        template <typename X, typename Y>
        void ReduceLongFunction<X, Y>::exec(const int opNum,
                             void *x,
                             Nd4jLong *xShapeInfo,
                             void *extraParams,
                             void *z,
                             Nd4jLong *resultShapeInfoBuffer,
                             int *dimension,
                             int dimensionLength,
                             Nd4jLong *tadShapeInfo,
                             Nd4jLong *tadOffset) {
                DISPATCH_BY_OPNUM_TT(exec, PARAMS(x, xShapeInfo, extraParams, z, resultShapeInfoBuffer, dimension, dimensionLength, tadShapeInfo, tadOffset), REDUCE_LONG_OPS);
        }

        template <typename X, typename Z>
        template <typename OpType>
        void _CUDA_H ReduceLongFunction<X,Z>::exec(void *vx,
                             Nd4jLong *xShapeInfo,
                             void *vextraParams,
                             void *vresult,
                             Nd4jLong *resultShapeInfoBuffer,
                             int *dimension,
                             int dimensionLength,
                             Nd4jLong *tadShapeInfo,
                             Nd4jLong *tadOffset) {

                auto x = reinterpret_cast<X *>(vx);
                auto z = reinterpret_cast<Z *>(vresult);
                auto extraParams = reinterpret_cast<X *>(vextraParams);

                auto resultLength = shape::length(resultShapeInfoBuffer);

                //pre squeezed: this is for keeping the pointer to the original
                //shape information for tad offset
                //the squeezed information doesn't render the right strides for
                //tad offset
                // || tad.wholeThing
                if (resultLength == 1 || dimension == nullptr || dimensionLength == shape::rank(xShapeInfo)) {
                    z[0] = execScalar<OpType>(x, xShapeInfo, extraParams);
                    return;
                }

                if (OpType::requiresSpecialAccumulation) {
                    OpType::execSpecial(x, xShapeInfo, extraParams, z, resultShapeInfoBuffer, dimension, dimensionLength, tadShapeInfo, tadOffset);
                    return;
                }

                auto tadOnlyShapeInfo = tadShapeInfo;
                auto tadOffsets = tadOffset;
                shape::TAD *tad = nullptr;

                if (tadOnlyShapeInfo == nullptr || tadOffsets == nullptr) {
                    tad = new shape::TAD();
                    tad->init(xShapeInfo, dimension, dimensionLength);
                    tad->createTadOnlyShapeInfo();
                    tad->createOffsets();

                    if (tad->dimensionLength < 1) {
                        delete tad;
                        return;
                    }

                    tadOnlyShapeInfo = tad->tadOnlyShapeInfo;
                    tadOffsets = tad->tadOffsets;
                }


                const auto tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
                auto numTads = shape::length(xShapeInfo) / tadLength;
                auto tadEWS = shape::elementWiseStride(tadOnlyShapeInfo);

                int tadsPerThread = resultLength / TAD_THRESHOLD;
                int num_threads = nd4j::math::nd4j_max<int>(1, tadsPerThread);
                num_threads = nd4j::math::nd4j_min<int>(num_threads, omp_get_max_threads());            

                uint castTadOnlyShapeInfo[MAX_RANK];
                const bool canCast = nd4j::DataTypeUtils::castShapeInfo<uint>(tadOnlyShapeInfo, castTadOnlyShapeInfo);

                PRAGMA_OMP_PARALLEL_FOR_THREADS(num_threads)
                for (int i = 0; i < resultLength; i++) {

                    auto tx = x + tadOffsets[i];
                    auto start = OpType::startingValue(tx);

                    for (int j = 0; j < tadLength; j++) {
                        auto xOffset = shape::indexOffset(j, tadOnlyShapeInfo, castTadOnlyShapeInfo, tadLength, canCast);
                        start = OpType::update(start, OpType::op(tx[xOffset], extraParams), extraParams);
                    }
                        
                    z[i] = OpType::postProcess(start, tadLength, extraParams);;
                }
                
                if (tad != nullptr)
                    delete tad;
            }


        template <typename X, typename Z>
        template<typename OpType>
        void _CUDA_H ReduceLongFunction<X,Z>::exec(void *x,
                             Nd4jLong *xShapeInfo,
                             void *extraParams,
                             void *vresult,
                             Nd4jLong *resultShapeInfo) {
                // FIXME: wtf???
                auto z = reinterpret_cast<Z*>(vresult);
                z[0] = execScalar<OpType>(x, xShapeInfo, extraParams);
        }

        template <typename X, typename Z>
        template <typename OpType>
        Z _CUDA_H ReduceLongFunction<X, Z>::execScalar(void *vx, Nd4jLong xEws, Nd4jLong length, void *vextraParams) {
                
                auto x = reinterpret_cast<X *>(vx);
                auto extraParams = reinterpret_cast<X *>(vextraParams);

                auto startingVal = OpType::startingValue(x);
                nd4j::OmpLaunchHelper info(length);

                if (xEws == 1) {

                    PRAGMA_OMP_PARALLEL_THREADS(info._numThreads)
                    {                
                        auto local = OpType::startingValue(x);
                        auto threadNum = omp_get_thread_num();                    
                        auto threadOffset = info.getThreadOffset(threadNum);
                        auto xi = x + threadOffset;
                        auto ulen = static_cast<unsigned int>(info.getItersPerThread(threadNum));

                        for (Nd4jLong i = 0; i < ulen; i++)
                            local = OpType::update(local, OpType::op(xi[i], extraParams), extraParams);

                        PRAGMA_OMP_CRITICAL
                        startingVal = OpType::update(startingVal, local, extraParams);        
                    }
                }
                else {

                    PRAGMA_OMP_PARALLEL_THREADS(info._numThreads)
                    {                
                        auto local = OpType::startingValue(x);
                        auto threadNum = omp_get_thread_num();                    
                        auto threadOffset = info.getThreadOffset(threadNum);
                        auto xi = x + xEws*threadOffset;
                        auto ulen = static_cast<unsigned int>(info.getItersPerThread(threadNum));

                        for (Nd4jLong i = 0; i < ulen; i++)
                            local = OpType::update(local, OpType::op(xi[i*xEws], extraParams), extraParams);

                        PRAGMA_OMP_CRITICAL
                        startingVal = OpType::update(startingVal, local, extraParams);        
                    }                    
                }
                return OpType::postProcess(startingVal, length, extraParams);
            }


        BUILD_DOUBLE_TEMPLATE(template class ND4J_EXPORT ReduceLongFunction, , LIBND4J_TYPES, LONG_TYPES);
    }
}