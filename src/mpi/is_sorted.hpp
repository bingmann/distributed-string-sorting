#pragma once

#include "mpi/allgather.hpp"
#include "mpi/allreduce.hpp"
#include "mpi/environment.hpp"
#include "mpi/gather.hpp"
#include "mpi/shift.hpp"
#include "strings/stringptr.hpp"
#include "strings/stringset.hpp"

#include <tlx/sort/strings/radix_sort.hpp>

namespace dss_schimek {

template <typename StringPtr>
std::vector<unsigned char> makeContiguous(StringPtr data) {
    std::vector<unsigned char> rawStrings;
    auto ss = data.active();
    for (auto str : ss) {
        auto chars = ss.get_chars(str, 0);
        auto length = ss.get_length(str);
        std::copy_n(chars, length + 1, std::back_inserter(rawStrings));
    }
    return rawStrings;
}

template <typename StringPtr>
class CheckerWithCompleteExchange {
    using StringSet = typename StringPtr::StringSet;

public:
    void storeLocalInput(
        const std::vector<unsigned char>& localInputRawStrings) {
        localInputRawStrings_ = localInputRawStrings;
    }

    std::vector<unsigned char> getLocalInput() { return localInputRawStrings_; }

    bool checkLcp() {
        dsss::mpi::environment env;

        bool correctSize = globalSortedLcps_.size() == globalInputLcps_.size();
        uint64_t counter = 0u;
        for (size_t i = 0; i < globalInputLcps_.size(); ++i) {
            if (globalInputLcps_[i] == globalSortedLcps_[i]) counter++;
        }
        std::cout << " checked lcp: " << std::endl;
        return correctSize && counter + env.size() >= globalSortedLcps_.size();
    }

    bool check(StringPtr sortedStrings, bool checkLcp_) {
        dsss::mpi::environment env;
        auto contigousSortedStrings = makeContiguous(sortedStrings);
        globalSortedRawStrings_ =
            dss_schimek::mpi::gatherv(contigousSortedStrings, 0);
        if (checkLcp_) {
            std::vector<uint64_t> localInput(sortedStrings.size());
            std::copy(sortedStrings.get_lcp(),
                sortedStrings.get_lcp() + sortedStrings.size(),
                localInput.begin());
            globalSortedLcps_ = dss_schimek::mpi::gatherv(localInput, 0);
        }
        gatherInput();
        if (env.rank() == 0u) {
            sortInputAndMakeContiguous();
            bool lcpsCorrect = true; 
            if (checkLcp_)
              lcpsCorrect = checkLcp();
            const bool sortedCorrectly =
                globalSortedRawStrings_ == globalInputRawStrings_;
            std::cout << globalSortedRawStrings_.size() << " "
                      << globalInputRawStrings_.size() << std::endl;
            bool overallCorrect = lcpsCorrect && sortedCorrectly;
            return dsss::mpi::allreduce_and(overallCorrect);
        }
        else {
            bool res = true;
            return dsss::mpi::allreduce_and(res);
        }
    }

private:
    std::vector<unsigned char> localInputRawStrings_;
    std::vector<unsigned char> globalInputRawStrings_;
    std::vector<uint64_t> globalInputLcps_;
    std::vector<unsigned char> globalSortedRawStrings_;
    std::vector<uint64_t> globalSortedLcps_;

    void gatherInput() {
        globalInputRawStrings_ =
            dss_schimek::mpi::gatherv(localInputRawStrings_, 0);
    }

    void sortInputAndMakeContiguous() {
        dss_schimek::StringLcpContainer<StringSet> container(
            std::move(globalInputRawStrings_));
        auto stringPtr = container.make_string_lcp_ptr();
        tlx::sort_strings_detail::radixsort_CI3(stringPtr, 0, 0);
        globalInputRawStrings_ = makeContiguous(stringPtr);
        globalInputLcps_ = std::move(container.lcps());
    }
};

template <typename StringPtr>
bool is_sorted(const StringPtr& strptr,
    dsss::mpi::environment env = dsss::mpi::environment()) {
    using StringSet = typename StringPtr::StringSet;

    const StringSet& ss = strptr.active();
    bool is_locally_sorted = ss.check_order();

    if (env.size() == 1) return is_locally_sorted;

    size_t has_strings = ss.size() > 0;
    size_t number_PE_with_data = dsss::mpi::allreduce_sum(has_strings);

    if (number_PE_with_data <= 1) return is_locally_sorted;

    int32_t own_min_number = has_strings ? env.rank() : env.size();
    int32_t own_max_number = has_strings ? env.rank() : -1;
    int32_t min_PE_with_data = dsss::mpi::allreduce_min(own_min_number);
    int32_t max_PE_with_data = dsss::mpi::allreduce_max(own_max_number);
    const bool is_left_shift = true;
    std::vector<unsigned char> greater_string;
    std::vector<unsigned char> smaller_string;
    unsigned char* front;
    unsigned char* back;
    if constexpr (std::is_same<StringSet, UCharLengthStringSet>::value) {
        front = has_strings ? (*ss.begin()).string : ss.empty_string().string;
        back =
            has_strings ? (*(ss.end() - 1)).string : ss.empty_string().string;
        greater_string = dss_schimek::mpi::shift_string<is_left_shift>(
            front, !has_strings, env);
        smaller_string = dss_schimek::mpi::shift_string<!is_left_shift>(
            back, !has_strings, env);
    }
    else {
        front = has_strings ? *ss.begin() : ss.empty_string();
        back = has_strings ? *(ss.end() - 1) : ss.empty_string();
        greater_string = dss_schimek::mpi::shift_string<is_left_shift>(
            front, !has_strings, env);
        smaller_string = dss_schimek::mpi::shift_string<!is_left_shift>(
            back, !has_strings, env);
    }
    if (false) {
        std::cout << "rank: " << env.rank() << " front: " << front
                  << " back: " << back
                  << " smaller_string: " << smaller_string.data()
                  << " greater_string: " << greater_string.data()
                  << " min_PE_with_data: " << min_PE_with_data
                  << " max_PE_with_data: " << max_PE_with_data << std::endl;
    }
    bool is_overall_sorted = is_locally_sorted;
    if (!has_strings) return dsss::mpi::allreduce_and(is_overall_sorted, env);

    if (static_cast<int64_t>(env.rank()) != min_PE_with_data)
        is_overall_sorted &=
            dss_schimek::scmp(smaller_string.data(), front) <= 0;
    if (static_cast<int64_t>(env.rank()) != max_PE_with_data)
        is_overall_sorted &=
            dss_schimek::scmp(back, greater_string.data()) <= 0;

    return dsss::mpi::allreduce_and(is_overall_sorted, env);
}

template <typename StringPtr>
bool is_complete_and_sorted(const StringPtr& strptr,
    size_t initial_local_num_chars, size_t current_local_num_chars,
    size_t initial_local_num_strings, size_t current_local_num_strings,
    dsss::mpi::environment env = dsss::mpi::environment()) {
    if (env.size() == 0) return is_sorted(strptr);

    const size_t initial_total_num_chars =
        dsss::mpi::allreduce_sum(initial_local_num_chars);
    const size_t initial_total_num_strings =
        dsss::mpi::allreduce_sum(initial_local_num_strings);

    const size_t current_total_num_chars =
        dsss::mpi::allreduce_sum(current_local_num_chars);
    const size_t current_total_num_strings =
        dsss::mpi::allreduce_sum(current_local_num_strings);

    if (initial_total_num_chars != current_total_num_chars) {
        std::cout << "initial total num chars: " << initial_total_num_chars
                  << " current_total_num_chars: " << current_total_num_chars
                  << std::endl;
        std::cout << "We've lost some chars" << std::endl;
        return false;
    }
    if (initial_total_num_strings != current_total_num_strings) {
        std::cout << "initial total num strings: " << initial_total_num_strings
                  << " current total num strings: " << current_total_num_strings
                  << std::endl;
        std::cout << "We've lost some strings" << std::endl;
        return false;
    }
    return is_sorted(strptr);
}
} // namespace dss_schimek
