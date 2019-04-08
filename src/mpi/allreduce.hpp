/*******************************************************************************
 * mpi/allreduce.hpp
 *
 * Copyright (C) 2018 Florian Kurpicz <florian.kurpicz@tu-dortmund.de>
 *
 * All rights reserved. Published under the BSD-2 license in the LICENSE file.
 ******************************************************************************/

#pragma once

#include <array>
#include <cmath>
#include <type_traits>
#include <vector>
#include <algorithm>

#include "mpi/environment.hpp"
#include "mpi/type_mapper.hpp"

#include "util/uint_types.hpp"

namespace dss_schimek::mpi {


template <typename DataType>
inline std::vector<DataType> allgatherForAllReduce(
    DataType& send_data, environment env = environment()) {
    data_type_mapper<DataType> dtm;
    std::vector<DataType> receive_data(env.size());
    MPI_Allgather(&send_data, 1, dtm.get_mpi_type(), receive_data.data(), 1,
        dtm.get_mpi_type(), env.communicator());
    return receive_data;
}

static inline bool allreduce_and(
    bool& send_data, environment env = environment()) {
    int send_data_ = send_data;
    int receive_data;
    MPI_Allreduce(
        &send_data_, &receive_data, 1, MPI_INT, MPI_LAND, env.communicator());
    return receive_data;
}

template <typename DataType>
static inline DataType allreduce_max(
    DataType& send_data, environment env = environment()) {
    static_assert(std::is_arithmetic<DataType>(),
        "Only arithmetic types are allowed for allreduce_max.");
    std::vector<DataType> elems = dss_schimek::mpi::allgatherForAllReduce(send_data);
    return *std::max_element(elems.begin(), elems.end());
}

template <typename DataType>
static inline DataType allreduce_min(
    DataType& send_data, environment env = environment()) {
    static_assert(std::is_arithmetic<DataType>(),
        "Only arithmetic types are allowed for allreduce_min.");
    auto elems = dss_schimek::mpi::allgatherForAllReduce(send_data);
    return *std::min_element(elems.begin(), elems.end());
}

template <typename DataType>
static inline DataType allreduce_sum(
    DataType& send_data, environment env = environment()) {
    static_assert(std::is_arithmetic<DataType>(),
        "Only arithmetic types are allowed for allreduce_sum.");

    auto elems = dss_schimek::mpi::allgatherForAllReduce(send_data);
    return std::accumulate(elems.begin(), elems.end(), 0ull);
}



} // namespace dss_schimek::mpi

/******************************************************************************/
