/*****************************************************************************
 * This file is part of the Project Karlsruhe Distributed Sorting Library
 * (KaDiS).
 *
 * Copyright (c) 2019, Michael Axtmann <michael.axtmann@kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/

#pragma once

#ifdef _OPENMP
#include <omp.h>
#endif

#include "util/measuringTool.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <numeric>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

#include "mpi/alltoall.hpp"
#include "mpi/environment.hpp"
#include "mpi/synchron.hpp"
#include "strings/stringcontainer.hpp"
#include "strings/stringset.hpp"
#include <tlx/sort/strings/radix_sort.hpp>

#include "../../include/RBC/RBC.hpp"
#include "ips4o.hpp"
#include "tlx/algorithm.hpp"
#include "tlx/math.hpp"

#include "../Tools/DummyTimer.hpp"
#include "./BinTreeMedianSelection.hpp"
#include "./RandomBitStore.hpp"

namespace RQuick {
namespace _internal {
constexpr bool debugQuicksort = false;
constexpr bool barrierActive = true;

uint64_t initialSize = 0;
inline void split(RBC::Comm& comm, RBC::Comm* subcomm) {
    const auto nprocs = comm.getSize();
    const auto myrank = comm.getRank();

    bool is_left_group = myrank < (nprocs / 2);

    int first = 0, last = 0;
    if (is_left_group) {
        first = 0;
        last = nprocs / 2 - 1;
    }
    else {
        first = nprocs / 2;
        last = nprocs - 1;
    }

    RBC::Comm_create_group(comm, subcomm, first, last);
}

/*
 * @brief Returns the k middle most elements.
 *
 * Returns the k middle most elements of v if v contains at least k
 * elements. Otherwise, v is returned. This function uses
 * randomization to decide which elements are the 'middle most'
 * elements if v contains an odd number of elements and k is an even
 * number or if v contains an even number of elements and k is an odd
 * number.
 */
template <class StringContainer>
std::vector<unsigned char> middleMostElements(StringContainer& cont, size_t k,
    std::mt19937_64& async_gen, RandomBitStore& bit_gen) {

    if (cont.size() <= k) {
        return std::vector<unsigned char>(cont.raw_strings());
    }

    const auto offset = (cont.size() - k) / 2;
    const bool shiftCondition = ((cont.size() % 2) == 0 && ((k % 2) == 0)) ||
                                (cont.size() % 2 == 1 && ((k % 2) == 1));
    int64_t shift = shiftCondition ? 0 : bit_gen.getNextBit(async_gen);
    const uint64_t begin = offset + shift;
    auto ss = cont.make_string_set();

    std::vector<unsigned char> middleMostRawStrings;
    if constexpr (debugQuicksort) {
        if (begin + k > cont.size()) {
            std::cout
                << "out of bounds error in middleMostElements, container size: "
                << cont.size() << " offset: " << offset << " k: " << k
                << " shift " << shift << std::endl;
            std::abort();
        }
    }
    for (size_t i = begin; i < begin + k; ++i) {
        const auto str = ss[ss.begin() + i];
        const auto length = ss.get_length(str) + 1;
        auto chars = ss.get_chars(str, 0);
        std::copy_n(chars, length, std::back_inserter(middleMostRawStrings));
    }
    return middleMostRawStrings;
}

/*
 * @brief Distributed splitter selection with a binary reduction tree.
 *
 * @param v Local input. The input must be sorted.
 */
template <class Comp, class StringContainer>
std::vector<unsigned char> selectSplitter(std::mt19937_64& async_gen,
    RandomBitStore& bit_gen, StringContainer& stringContainer,
    MPI_Datatype mpi_type, Comp&& comp, int tag, RBC::Comm& comm) {

    // assert StringContainer is sorted
    if constexpr (debugQuicksort) {
        if (!stringContainer.isConsistent()) {
            std::cout << "corrupt string cont" << std::endl;
            std::abort();
        }
    }
    // dss_schimek::mpi::environment env;
    std::vector<unsigned char> local_medians =
        middleMostElements(stringContainer, 2, async_gen, bit_gen);
    //    auto local_mediansCopy = local_medians;

    //    StringContainer localMedians(std::move(local_mediansCopy));
    //    // std::cout << "rank: " << comm.getRank()
    //    //          << " print splitter size: " << localMedians.size()
    //    //          << std::endl;
    //    localMedians.make_string_set().print();

    //    if (!localMedians.isConsistent() ||
    //        localMedians.char_size() > stringContainer.char_size()) {
    //        std::cout << " corrupt local medians" << std::endl;
    //        std::abort();
    //    }
    std::vector<unsigned char> res = BinTreeMedianSelection::select(
        local_medians.data(), local_medians.data() + local_medians.size(), 2,
        std::forward<Comp>(comp), mpi_type, async_gen, bit_gen, tag, comm);
    if constexpr (debugQuicksort) {
        if (res.size() < 1 || res.back() != 0) {
            std::cout << "error in final median" << std::endl;
            std::abort();
        }
    }
    return res;
}

/*
 * @brief Split vector according to a given splitter with or without
 * tie-breaking.
 *
 * No tie-breaking: Returns a pointer to the first element which is
 * larger or equal to the splitter.
 *
 * With tie-breaking: Splits the vector according to a given splitter
 * such that the split is as close to the middle of the vector as
 * possible.
 *
 * @param v Local input. The input must be sorted.
 */
template <class T, class Comp>
const T* locateSplitter(const std::vector<T>& v, Comp&& comp, const T& splitter,
    std::mt19937_64& gen, RandomBitStore& bit_store, bool is_robust) {
    const auto begin_equal_els = std::lower_bound(
        v.data(), v.data() + v.size(), splitter, std::forward<Comp>(comp));
    if (!is_robust) {
        return begin_equal_els;
    }

    const auto end_equal_els = std::upper_bound(begin_equal_els,
        v.data() + v.size(), splitter, std::forward<Comp>(comp));

    // Round down or round up randomly.
    const auto opt_split = v.data() + v.size() / 2 +
                           ((v.size() % 2) == 1 && bit_store.getNextBit(gen));

    if (begin_equal_els < opt_split) {
        return std::min(opt_split, end_equal_els);
    }
    else {
        return begin_equal_els;
    }
}

/*
 * @brief Partitions two sequences according to a specific rank.
 *
 * Split two sequences into a left and a right part each. The left
 * parts contain 'rank' elements in total and the left parts only
 * contain elements which are smaller than elements of the right
 * parts. Tie-breaking is used meaning that elements of the first
 * sequence are smaller than the same elements of the second sequence
 * and equal elements of the same sequence use their index as
 * tie-breaker.
 *
 * @param begin1 Begin of the first sorted sequence.
 * @param end1 End of the first sorted sequence.
 * @param begin2 Begin of the second sorted sequence.
 * @param end2 End of the second sorted sequence.
 * @param rank Total size of the left part. Rank must be a positive
 *  number smaller of equal to the total size of both input sequences.
 * @param comp The comparator.
 */
template <class T, class Comp>
std::pair<const T*, const T*> twoSequenceSelection(const T* begin1,
    const T* end1, const T* begin2, const T* end2, int64_t rank, Comp&& comp) {
    assert(std::is_sorted(begin1, end1, std::forward<Comp>(comp)));
    assert(std::is_sorted(begin2, end2, std::forward<Comp>(comp)));

    assert(static_cast<size_t>((end1 - begin1) + (end2 - begin2)) >= rank);

    // Shrink sequences by taking splitters from the first
    // sequence until first sequence does not contain any
    // elements.
    while (end1 != begin1) {
        assert(end1 > begin1);

        const auto offset1 = (end1 - begin1) / 2;
        const auto splitter1 = begin1 + offset1;
        assert(begin1 <= splitter1);
        assert(splitter1 < end1);

        // We use tie-breaking. Meaning than an element 'a' in
        // the first sequence is smaller than the same element
        // 'a' in the second sequence. Thus, the function
        // 'lower_bound' breaks ties implicitly.
        const auto splitter2 = std::lower_bound(
            begin2, end2, *splitter1, std::forward<Comp>(comp));
        const auto offset2 = splitter2 - begin2;

        const auto new_rank = offset1 + offset2;

        if (rank < new_rank) {
            // Continue on left part.

            // Size of first sequence decreases by at least
            // one as 'splitter1' is now excluded.

            end1 = splitter1;
            end2 = splitter2;
        }
        else if (rank > new_rank) {
            // Continue on right part.

            // Size of the first sequence decreases by at least one
            // (even if the frist element is selected as 'splitter1')
            // as 'splitter1' is now excluded. We can exclude
            // 'splitter1' from the first sequence, as all elements in
            // [splitter2, end2) as they are larger than 'splitter1'
            // (ties are already broken). However, as 'rank >
            // new_rank', we can remove the smallest element.

            rank -= new_rank + 1;
            begin1 = splitter1 + 1;
            begin2 = splitter2;
        }
        else {
            return {splitter1, splitter2};
        }
    }

    // Position in first sequence has been found. We still
    // need 'rank' elements. Those 'rank' elements must be the
    // first elements of the second sequence.
    return {begin1, begin2 + rank};
}

template <class T>
void exchange(const T* send_begin, const T* send_end, std::vector<T>& v_recv,
    int target, MPI_Datatype mpi_type, int tag, RBC::Comm& comm) {
    const auto send_size = send_end - send_begin;

    RBC::Request requests[2];
    RBC::Isend(send_begin, send_size, mpi_type, target, tag, comm, requests);

    int recv_size = 0;
    MPI_Status status;
    RBC::Probe(target, tag, comm, &status);
    MPI_Get_count(&status, mpi_type, &recv_size);

    v_recv.resize(recv_size);
    RBC::Irecv(
        v_recv.data(), recv_size, mpi_type, target, tag, comm, requests + 1);
    RBC::Waitall(2, requests, MPI_STATUSES_IGNORE);
}

template <class T, class Comp>
void merge(const T* begin1, const T* end1, const T* begin2, const T* end2,
    T* tbegin, Comp&& comp) {
    assert(std::is_sorted(begin1, end1, std::forward<Comp>(comp)));
    assert(std::is_sorted(begin2, end2, std::forward<Comp>(comp)));

#ifdef _OPENMP

    const auto num_elements = (end1 - begin1) + (end2 - begin2);

#pragma omp parallel
    {
        const auto num_threads = omp_get_num_threads();
        const auto id = omp_get_thread_num();

        const auto stripe = tlx::div_ceil(num_elements, num_threads);

        const auto rank_begin = std::min(id * stripe, num_elements);
        const auto rank_end = std::min((id + 1) * stripe, num_elements);

        auto [s1_begin, s2_begin] = twoSequenceSelection(
            begin1, end1, begin2, end2, rank_begin, std::forward<Comp>(comp));
        assert((s1_begin - begin1) + (s2_begin - begin2) == rank_begin);

        auto [s1_end, s2_end] = twoSequenceSelection(s1_begin, end1, s2_begin,
            end2, rank_end - rank_begin, std::forward<Comp>(comp));
        assert(s1_begin <= s1_end);
        assert(s2_begin <= s2_end);
        assert((s1_end - begin1) + (s2_end - begin2) == rank_end);

        std::merge(s1_begin, s1_end, s2_begin, s2_end, tbegin + rank_begin,
            std::forward<Comp>(comp));
    }

#else

    std::merge(begin1, end1, begin2, end2, tbegin, std::forward<Comp>(comp));

#endif
}

template <class Tracker, class Comp, class StringContainer>
dss_schimek::StringContainer<dss_schimek::UCharLengthStringSet> sortRec(
    std::mt19937_64& gen, RandomBitStore& bit_store,
    StringContainer&& stringContainer, Comp&& comp, MPI_Datatype mpi_type,
    bool is_robust, Tracker&& tracker, int tag, RBC::Comm& comm) {

    using StringSet = typename StringContainer::StringSet;
    using String = typename StringSet::String;
    using dss_schimek::measurement::MeasuringTool;
    static uint64_t iteration = 0;
    ++iteration;
    MeasuringTool& measuringTool = MeasuringTool::measuringTool();
    measuringTool.setRound(iteration);
    if constexpr (barrierActive) {
        measuringTool.start("Splitter_median_select_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_median_select_Barrier");
    }
    measuringTool.start("Splitter_median_select");
    tracker.median_select_t.start(comm);

    const auto nprocs = comm.getSize();
    const auto myrank = comm.getRank();

    assert(nprocs >= 2);
    assert(std::is_sorted(v.begin(), v.end(), std::forward<Comp>(comp)));
    assert(tlx::integer_log2_floor(nprocs));

    const auto is_left_group = myrank < nprocs / 2;

    // Select pivot globally with binary tree median selection.
    std::vector<unsigned char> pivot = selectSplitter(gen, bit_store,
        stringContainer, mpi_type, std::forward<Comp>(comp), tag, comm);

    tracker.median_select_t.stop();
    String pivotString(pivot.data(), pivot.size() - 1);
    measuringTool.stop("Splitter_median_select");

    // Partition data into small elements and large elements.

    if constexpr (barrierActive) {
        measuringTool.start("Splitter_partition_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_partition_Barrier");
    }
    measuringTool.start("Splitter_partition");
    tracker.partition_t.start(comm);

    const auto* separator = locateSplitter(stringContainer.getStrings(),
        std::forward<Comp>(comp), pivotString, gen, bit_store, is_robust);

    if constexpr (debugQuicksort) {
        if (separator < stringContainer.getStrings().data() ||
            separator >
                stringContainer.getStrings().data() + stringContainer.size()) {
            std::cout << "error in locate splitter" << std::endl;
            std::abort();
        }
    }

    if constexpr (debugQuicksort) {
        const uint64_t partitionSize =
            separator - stringContainer.getStrings().data();
        std::cout << "rank: " << comm.getRank() << " size: " << partitionSize
                  << " " << stringContainer.size() - partitionSize << std::endl;
    }

    String* send_begin = stringContainer.getStrings().data();
    String* send_end = const_cast<String*>(separator);
    String* own_begin = const_cast<String*>(separator);
    String* own_end = stringContainer.getStrings().data() +
                      stringContainer.getStrings().size();
    if (is_left_group) {
        std::swap(send_begin, own_begin);
        std::swap(send_end, own_end);
    }

    uint64_t sendCounts = 0;
    for (auto i = send_begin; i < send_end; ++i) {
        sendCounts += i->getLength() + 1;
    }
    std::vector<unsigned char> sendCharsContiguous(sendCounts);
    uint64_t curPos = 0;
    for (auto i = send_begin; i < send_end; ++i) {
        auto length = i->getLength() + 1;
        auto chars = i->getChars();
        std::copy_n(chars, length, sendCharsContiguous.begin() + curPos);
        curPos += length;
    }
    const uint64_t ownCharsSize =
        stringContainer.char_size() - sendCharsContiguous.size();

    uint64_t inbalance = std::abs(
        static_cast<int64_t>(stringContainer.size()) - (send_end - send_begin));
    measuringTool.add(inbalance, "inbalance", false);

    tracker.partition_t.stop();
    measuringTool.stop("Splitter_partition");

    // Move elements to partner and receive elements for own group.
    tracker.exchange_t.start(comm);
    if constexpr (barrierActive) {
        measuringTool.start("Splitter_exchange_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_exchange_Barrier");
    }
    measuringTool.start("Splitter_exchange");

    const auto partner = (myrank + (nprocs / 2)) % nprocs;
    std::vector<unsigned char> recvRawStrings;
    exchange(sendCharsContiguous.data(),
        sendCharsContiguous.data() + sendCharsContiguous.size(), recvRawStrings,
        partner, mpi_type, tag, comm);

    StringContainer recvStrings(std::move(recvRawStrings));

    tracker.exchange_t.stop();
    measuringTool.stop("Splitter_exchange");
    // Merge received elements with own elements.
    tracker.merge_t.start(comm);
    if constexpr (barrierActive) {
        measuringTool.start("Splitter_merge_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_merge_Barrier");
    }
    measuringTool.start("Splitter_merge");

    const auto num_elements = recvStrings.size() + (own_end - own_begin);
    std::vector<String> mergedStrings(num_elements);
    merge(own_begin, own_end, recvStrings.getStrings().begin(),
        recvStrings.getStrings().end(), mergedStrings.begin(),
        std::forward<Comp>(comp));
    std::vector<unsigned char> mergedRawStrings(
        recvStrings.char_size() + ownCharsSize);

    curPos = 0;
    for (auto str : mergedStrings) {
        auto length = str.getLength() + 1;
        auto chars = str.getChars();
        std::copy_n(chars, length, mergedRawStrings.begin() + curPos);
        curPos += length;
    }
    stringContainer.update(std::move(mergedRawStrings));
    measuringTool.stop("Splitter_merge");
    if constexpr (debugQuicksort) {
        if (!stringContainer.isConsistent()) {
            std::cout << "merged string cont not consistent " << std::endl;
            std::abort();
        }
    }

    if (nprocs >= 4) {
        // Split communicator and solve subproblems.
    if constexpr (barrierActive) {
        measuringTool.start("Splitter_split_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_split_Barrier");
    }
        measuringTool.start("Splitter_split");
        tracker.comm_split_t.start(comm);

        RBC::Comm subcomm;
        split(comm, &subcomm);

        tracker.comm_split_t.stop();
        measuringTool.stop("Splitter_split");

        auto res = sortRec(gen, bit_store, std::move(stringContainer),
            std::forward<Comp>(comp), mpi_type, is_robust,
            std::forward<Tracker>(tracker), tag, subcomm);
        measuringTool.disableBarrier(false);
        measuringTool.setRound(0);
        return res;
    }
    measuringTool.disableBarrier(false);
    measuringTool.setRound(0);
    return std::move(stringContainer);
}

template <class T, class Ignore>
void shuffle(std::mt19937_64& async_gen, std::vector<T>& v,
    std::vector<Ignore>& v_tmp, MPI_Datatype mpi_type, int tag,
    RBC::Comm& comm) {
    // Just used for OpenMP
    std::ignore = v_tmp;

    const size_t nprocs = comm.getSize();
    const size_t myrank = comm.getRank();

    // Generate a random bit generator for each thread. Using pointers
    // is faster than storing the generators in one vector due to
    // cache-line sharing.
#ifdef _OPENMP
    int num_threads = 1;
    std::vector<std::unique_ptr<std::mt19937_64>> omp_async_gens;
    std::vector<size_t> seeds;
#pragma omp parallel
    {
#pragma omp single
        {
            num_threads = omp_get_num_threads();
            omp_async_gens.resize(num_threads);
            for (int i = 0; i != num_threads; ++i) {
                seeds.push_back(async_gen());
            }
        }

        const int id = omp_get_thread_num();

        omp_async_gens[id].reset(new std::mt19937_64{seeds[id]});
    }

    std::vector<size_t> sizes(2 * num_threads);
    std::vector<size_t> prefix_sum(2 * num_threads + 1);

#endif

    const size_t comm_phases = tlx::integer_log2_floor(nprocs);

    for (size_t phase = 0; phase != comm_phases; ++phase) {
        const size_t mask = 1 << phase;
        const size_t partner = myrank ^ mask;

#ifdef _OPENMP

        int send_cnt = 0;
        int own_cnt = 0;
        int recv_cnt = 0;

#pragma omp parallel
        {
            const int id = omp_get_thread_num();

            const size_t stripe = tlx::div_ceil(v.size(), num_threads);
            std::unique_ptr<T[]> partitions[2] = {
                std::unique_ptr<T[]>(new T[stripe]),
                std::unique_ptr<T[]>(new T[stripe])};

            const auto begin = v.data() + id * stripe;
            const auto end = v.data() + std::min((id + 1) * stripe, v.size());

            auto& mygen = *omp_async_gens[id].get();
            T* partptrs[2] = {partitions[0].get(), partitions[1].get()};

            const auto size = end - begin;
            const auto remaining =
                size % (8 * sizeof(std::mt19937_64::result_type));
            auto ptr = begin;
            while (ptr < end - remaining) {
                auto rand = mygen();
                for (size_t j = 0; j < 8 * sizeof(std::mt19937_64::result_type);
                     ++j) {
                    const auto bit = rand & 1;
                    rand = rand >> 1;
                    *partptrs[bit] = *ptr;
                    ++partptrs[bit];
                    ++ptr;
                }
            }

            auto rand = mygen();
            while (ptr < end) {
                // for (size_t i = 0; i != remaining; ++i) {
                const auto bit = rand & 1;
                rand = rand >> 1;
                *partptrs[bit] = *ptr;
                ++partptrs[bit];
                ++ptr;
            }

            sizes[id] = partptrs[0] - partitions[0].get();
            sizes[num_threads + id] = partptrs[1] - partitions[1].get();

#pragma omp barrier
#pragma omp master
            {
                tlx::exclusive_scan(
                    sizes.begin(), sizes.end(), prefix_sum.begin(), size_t{0});

                send_cnt =
                    prefix_sum[2 * num_threads] - prefix_sum[num_threads];
                own_cnt = prefix_sum[num_threads];
                recv_cnt = 0;
                RBC::Sendrecv(&send_cnt, 1, MPI_INT, partner, tag, &recv_cnt, 1,
                    MPI_INT, partner, tag, comm, MPI_STATUS_IGNORE);

                v.resize(own_cnt + recv_cnt);
                v_tmp.resize(send_cnt);
            }
#pragma omp barrier

            std::copy(partitions[0].get(), partitions[0].get() + sizes[id],
                v.data() + prefix_sum[id]);
            std::copy(partitions[1].get(),
                partitions[1].get() + sizes[num_threads + id],
                v_tmp.data() + prefix_sum[id + num_threads] -
                    prefix_sum[num_threads]);
        }

        RBC::Sendrecv(v_tmp.data(), send_cnt, mpi_type, partner, tag,
            v.data() + own_cnt, recv_cnt, mpi_type, partner, tag, comm,
            MPI_STATUS_IGNORE);

#else
        // v contains stringIndices
        std::unique_ptr<T[]> partition(new T[v.size()]);
        T* begins[2] = {v.data(), partition.get()};

        const auto size = v.size();
        const auto remaining =
            size % (8 * sizeof(std::mt19937_64::result_type));
        auto ptr = v.begin();
        const auto end = v.end();
        while (ptr < end - remaining) {
            auto rand = async_gen();
            for (size_t j = 0; j < 8 * sizeof(std::mt19937_64::result_type);
                 ++j) {
                const auto bit = rand & 1;
                rand = rand >> 1;
                *begins[bit] = *ptr;
                ++begins[bit];
                ++ptr;
            }
        }

        auto rand = async_gen();
        while (ptr < end) {
            const auto bit = rand & 1;
            rand = rand >> 1;
            *begins[bit] = *ptr;
            ++begins[bit];
            ++ptr;
        }

        RBC::Request requests[2];
        // send sizes
        RBC::Isend(partition.get(), (begins[1] - partition.get()) * sizeof(T),
            mpi_type, partner, tag, comm, requests);

        int count = 0;
        MPI_Status status;
        RBC::Probe(partner, tag, comm, &status);
        MPI_Get_count(&status, mpi_type, &count);
        count /= sizeof(T);

        v.resize(
            begins[0] - v.data() + count); // begins[0] end of string indices

        RBC::Irecv(v.data() + v.size() - count, count * sizeof(T), mpi_type,
            partner, tag, comm, requests + 1);
        RBC::Waitall(2, requests, MPI_STATUSES_IGNORE);

#endif
    }
}

template <typename StringPtr>
void sortLocally(StringPtr strptr) {
    tlx::sort_strings_detail::radixsort_CI3(strptr, 0, 0);
}

template <class Iterator, class Comp>
void sortLocally(Iterator begin, Iterator end, Comp&& comp) {
#ifdef _OPENMP
    ips4o::parallel::sort(begin, end, std::forward<Comp>(comp));
#else
    ips4o::sort(begin, end, std::forward<Comp>(comp));
#endif
}

template <class Tracker, class T, class Comp>
dss_schimek::StringContainer<dss_schimek::UCharLengthStringSet> sort(
    std::mt19937_64& async_gen, std::vector<T>& v, MPI_Datatype mpi_type,
    int tag, RBC::Comm comm, Tracker&& tracker, Comp&& comp, bool is_robust) {
    using StringSet = dss_schimek::UCharLengthStringSet;
    using StringContainer = dss_schimek::StringContainer<StringSet>;
    using dss_schimek::measurement::MeasuringTool;

    MeasuringTool& measuringTool = MeasuringTool::measuringTool();
    measuringTool.disableBarrier(true);
    if constexpr (barrierActive) {
        measuringTool.start("Splitter_baseCase_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_baseCase_Barrier");
    }
    measuringTool.start("Splitter_baseCase");
    if (comm.getSize() == 1) {
        StringContainer container(std::move(v));
        tracker.local_sort_t.start(comm);

        sortLocally(container.make_string_ptr());
        tracker.local_sort_t.stop();
        return container;
    }
    measuringTool.stop("Splitter_baseCase");

    if constexpr (barrierActive) {
        measuringTool.start("Splitter_move_to_pow_of_two_t_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_move_to_pow_of_two_t_Barrier");
    }
    measuringTool.start("Splitter_move_to_pow_of_two_t");
    tracker.move_to_pow_of_two_t.start(comm);

    const auto pow = tlx::round_down_to_power_of_two(comm.getSize());

    // Send data to a smaller hypercube if the number of processes is
    // not a power of two.
    if (comm.getRank() < comm.getSize() - pow) {
        // Not a power of two but we are part of the smaller hypercube
        // and receive elements.

        MPI_Status status;
        const auto source = pow + comm.getRank();

        RBC::Probe(source, tag, comm, &status);
        int recv_cnt = 0;
        MPI_Get_count(&status, mpi_type, &recv_cnt);

        // Avoid reallocations later.
        v.reserve(2 * (v.size() + recv_cnt));

        v.resize(v.size() + recv_cnt);
        MPI_Request request;
        RBC::Irecv(v.data() + v.size() - recv_cnt, recv_cnt, mpi_type, source,
            tag, comm, &request);
        MPI_Wait(&request, MPI_STATUS_IGNORE);

        RBC::Comm sub_comm;
        RBC::Comm_create_group(comm, &sub_comm, 0, pow - 1);
        comm = sub_comm;
    }
    else if (comm.getRank() >= pow) {
        // Not a power of two and we are not part of the smaller
        // hypercube.

        const auto target = comm.getRank() - pow;
        RBC::Send(v.data(), v.size(), mpi_type, target, tag, comm);
        v.clear();

        // This process is not part of 'sub_comm'. We call
        // this function to support MPI implementations
        // without MPI_Comm_create_group.
        RBC::Comm sub_comm;
        RBC::Comm_create_group(comm, &sub_comm, 0, pow - 1);
        comm = sub_comm;

        tracker.move_to_pow_of_two_t.stop();

        return StringContainer();
    }
    else if (pow != comm.getSize()) {
        // Not a power of two but we are part of the smaller hypercube
        // and do not receive elements.

        RBC::Comm sub_comm;
        RBC::Comm_create_group(comm, &sub_comm, 0, pow - 1);
        comm = sub_comm;

        // Avoid reallocations later.
        v.reserve(3 * v.size());
    }
    else {
        // The number of processes is a power of two.

        // Avoid reallocations later.
        v.reserve(2 * v.size());
    }

    StringContainer container(std::move(v));
    measuringTool.stop("Splitter_move_to_pow_of_two_t");
    tracker.move_to_pow_of_two_t.stop();

    assert(tlx::is_power_of_two(comm.getSize()));

    if constexpr (barrierActive) {
        measuringTool.start("splitter_shuffle_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("splitter_shuffle_Barrier");
    }
    tracker.parallel_shuffle_t.start(comm);
    measuringTool.start("Splitter_shuffle");

    // Vector is used to store about the same number of elements as v.
    std::vector<T> tmp1;
    tmp1.reserve(v.capacity());

    // Vector is used to store about half the number of elements as
    // v. This vector is used to receive data.
    std::vector<T> tmp2;
    tmp2.reserve(v.capacity() / 2);

    // if (is_robust) {
    //    using StringIndexPEIndex = std::pair<uint64_t, uint64_t>;
    //    std::vector<StringIndexPEIndex> stringIndicesPEIndices(
    //        container.size());
    //    const auto rank = comm.getRank();
    //    for (size_t i = 0; i < container.size(); ++i) {
    //        stringIndicesPEIndices[i].first = i;
    //        stringIndicesPEIndices[i].second = rank;
    //    }
    //    // for (size_t i = 0; i < stringIndicesPEIndices.size(); ++i) {
    //    //    std::cout << "rank: " << rank << " ("
    //    //              << stringIndicesPEIndices[i].first << ", "
    //    //              << stringIndicesPEIndices[i].second << std::endl;
    //    //}
    //    shuffle(async_gen, stringIndicesPEIndices, tmp2, MPI_BYTE, tag, comm);
    //    // for (size_t i = 0; i < stringIndicesPEIndices.size(); ++i) {
    //    //    std::cout << "rank: " << rank << " ("
    //    //              << stringIndicesPEIndices[i].first << ", "
    //    //              << stringIndicesPEIndices[i].second << ")" <<
    //    //std::endl;
    //    //}
    //    // alltoall string exchange
    //    std::vector<uint64_t> elemsFromRanks(comm.getSize(), 0);
    //    std::vector<uint64_t> prefixSum;
    //    prefixSum.reserve(comm.getSize());
    //    for (const auto& elem : stringIndicesPEIndices)
    //        ++elemsFromRanks[elem.second];
    //    std::partial_sum(elemsFromRanks.begin(), elemsFromRanks.end(),
    //        std::back_inserter(prefixSum));
    //    std::vector<uint64_t> elemsFromRanksCopy(elemsFromRanks);
    //    std::vector<uint64_t> requests(stringIndicesPEIndices.size());
    //    for (const auto& elem : stringIndicesPEIndices) {
    //        const auto requestIndex =
    //            prefixSum[elem.second] - (elemsFromRanksCopy[elem.second]--);
    //        requests[requestIndex] = elem.first;
    //    }
    //    // for (size_t i = 0; i < requests.size(); ++i) {
    //    //    std::cout << "rank: " << rank << " " << requests[i] <<
    //    //std::endl;
    //    //}
    //    dss_schimek::mpi::environment env;
    //    env.setCommunicator(comm.get());
    //    auto recvRequestSizes = dss_schimek::mpi::alltoall(elemsFromRanks);
    //    // for (size_t i = 0; i < comm.getSize(); ++i) {
    //    //  std::cout << " rank: " << rank << " received " <<
    //    //  recvRequestSizes[i] << " from " << i << std::endl;
    //    //}
    //    using AllToAllv = dss_schimek::mpi::AllToAllvCombined<
    //        dss_schimek::mpi::AllToAllvSmall>;
    //    auto recvRequests =
    //        AllToAllv::alltoallv(requests.data(), elemsFromRanks, env);

    //    std::vector<uint64_t> sendCharCounts(comm.getSize(), 0);

    //    StringSet ss = container.make_string_set();
    //    std::vector<unsigned char> sendRawStrings(container.char_size());
    //    uint64_t curPos = 0;
    //    uint64_t curRequest = 0;
    //    for (int64_t i = 0; i < comm.getSize(); ++i) {
    //        for (size_t j = 0; j < recvRequestSizes[i]; ++j, ++curRequest) {
    //            // std::cout << "rank: " << rank << " " <<
    //            // recvRequests[curRequest] << " size: " << ss.size() <<
    //            // std::endl;
    //            const auto curString =
    //                ss[ss.begin() + recvRequests[curRequest]];
    //            const auto length = ss.get_length(curString) + 1;
    //            const auto chars = ss.get_chars(curString, 0);
    //            std::copy(
    //                chars, chars + length, sendRawStrings.data() + curPos);
    //            curPos += length;
    //            sendCharCounts[i] += length;
    //        }
    //    }

    //    auto recvRawStrings =
    //        AllToAllv::alltoallv(sendRawStrings.data(), sendCharCounts, env);
    //    container.update(std::move(recvRawStrings));
    //    // std::cout << "----------" << std::endl;
    //}

    measuringTool.stop("Splitter_shuffle");
    tracker.parallel_shuffle_t.stop();

    if constexpr (barrierActive) {
        measuringTool.start("Splitter_sortLocally_Barrier");
        RBC::Barrier(comm);
        measuringTool.stop("Splitter_sortLocally_Barrier");
    }
    measuringTool.start("Splitter_sortLocally");
    tracker.local_sort_t.start(comm);
    sortLocally(container.make_string_ptr());
    tracker.local_sort_t.stop();
    measuringTool.stop("Splitter_sortLocally");
    // std::vector<unsigned char> matthiasTmp;
    // auto ss = container.make_string_set();
    // for (size_t i = 0; i < ss.size(); ++i) {
    //    const auto str = ss[ss.begin() + i];
    //    const auto length = ss.get_length(str) + 1;
    //    auto chars = ss.get_chars(str, 0);
    //    std::copy(chars, chars + length, std::back_inserter(matthiasTmp));
    //    ;
    //}
    // container.update(std::move(matthiasTmp));

    // return StringContainer();
    RandomBitStore bit_store;
    return _internal::sortRec(async_gen, bit_store, std::move(container),
        std::forward<Comp>(comp), mpi_type, is_robust,
        std::forward<Tracker>(tracker), tag, comm);
}

class DummyTracker {
public:
    Tools::DummyTimer local_sort_t;
    Tools::DummyTimer exchange_t;
    Tools::DummyTimer parallel_shuffle_t;
    Tools::DummyTimer merge_t;
    Tools::DummyTimer median_select_t;
    Tools::DummyTimer partition_t;
    Tools::DummyTimer comm_split_t;
    Tools::DummyTimer move_to_pow_of_two_t;
};
} // namespace _internal

template <class Tracker, class T, class Comp>
dss_schimek::StringContainer<dss_schimek::UCharLengthStringSet> sort(
    Tracker&& tracker, std::mt19937_64& async_gen, std::vector<T>& v,
    MPI_Datatype mpi_type, int tag, MPI_Comm mpi_comm, Comp&& comp,
    bool is_robust) {
    RBC::Comm comm;
    RBC::Create_Comm_from_MPI(mpi_comm, &comm);
    return _internal::sort(async_gen, v, mpi_type, tag, comm,
        std::forward<Tracker>(tracker), std::forward<Comp>(comp), is_robust);
}

template <class T, class Comp>
dss_schimek::StringContainer<dss_schimek::UCharLengthStringSet> sort(
    std::mt19937_64& async_gen, std::vector<T>& v, MPI_Datatype mpi_type,
    int tag, MPI_Comm mpi_comm, Comp&& comp, bool is_robust) {
    _internal::DummyTracker tracker;
    return sort(
        tracker, async_gen, v, mpi_type, tag, mpi_comm, comp, is_robust);
}
//
// template <class Tracker, class T, class Comp>
// void sort(Tracker&& tracker, std::mt19937_64& async_gen, std::vector<T>& v,
//    MPI_Datatype mpi_type, int tag, RBC::Comm& comm, Comp&& comp,
//    bool is_robust) {
//    _internal::sort(async_gen, v, mpi_type, tag, comm,
//        std::forward<Tracker>(tracker), std::forward<Comp>(comp), is_robust);
//}
//
// template <class T, class Comp>
// void sort(std::mt19937_64& async_gen, std::vector<T>& v, MPI_Datatype
// mpi_type,
//    int tag, RBC::Comm& comm, Comp&& comp, bool is_robust) {
//    _internal::DummyTracker tracker;
//    sort(tracker, async_gen, v, mpi_type, tag, comm, comp, is_robust);
//}
} // namespace RQuick
