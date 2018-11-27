#include <algorithm>
#include "strings/stringptr.hpp"
#include "strings/stringset.hpp"
#include "strings/stringtools.hpp"

#include "sorter/local/strings/insertion_sort_unified.hpp"
#include "sorter/local/strings/multikey_quicksort_unified.hpp"

#include "mpi/alltoall.hpp"
#include "mpi/allgather.hpp"
#include "mpi/synchron.hpp"
#include "mpi/is_sorted.hpp"

#include "merge/stringtools.hpp"
#include "merge/bingmann-lcp_losertree.hpp"

#include "util/timer.hpp"

namespace dss_schimek {

  static constexpr bool debug = false;

  template <typename StringSet>
    class  SampleSplittersNumStringsPolicy
    {
      protected:
        std::vector<typename StringSet::Char> sample_splitters(const StringSet& ss,
            dsss::mpi::environment env = dsss::mpi::environment()) {

          using Char = typename StringSet::Char;
          using String = typename StringSet::String;

          const size_t local_num_strings = ss.size();
          const size_t nr_splitters = std::min<size_t>(env.size() - 1, local_num_strings);
          const size_t splitter_dist = local_num_strings / (nr_splitters + 1);
          std::vector<Char> raw_splitters;

          for (size_t i = 1; i <= nr_splitters; ++i) {
            const String splitter = ss[ss.begin() + i * splitter_dist];
            std::copy_n(ss.get_chars(splitter, 0), ss.get_length(splitter) + 1,
                std::back_inserter(raw_splitters));
          }
          return raw_splitters; 
        }
    };

  template <typename StringSet>
  class  SampleSplittersNumCharsPolicy
  {
     protected:
        std::vector<typename StringSet::Char> sample_splitters(const StringSet& ss,
            dsss::mpi::environment env = dsss::mpi::environment()) {

          using Char = typename StringSet::Char;
          using String = typename StringSet::String;

          const size_t num_chars = std::accumulate(ss.begin(), ss.end(), 0, 
              [&ss](const size_t& sum, const String& str) {
              return sum + ss.get_length(str);
              });

          const size_t local_num_strings = ss.size();
          const size_t nr_splitters = std::min<size_t>(env.size() - 1, local_num_strings);
          const size_t splitter_dist = num_chars / (nr_splitters + 1);
          std::vector<Char> raw_splitters;

          size_t string_index = 0;
          for (size_t i = 1; i <= nr_splitters; ++i) {
            size_t num_chars_seen = 0;
            while (num_chars_seen < splitter_dist) {
              num_chars_seen += ss.get_length(ss[ss.begin() + string_index]);
              ++string_index;
            }

            const String splitter = ss[ss.begin() + string_index - 1];
            std::copy_n(ss.get_chars(splitter, 0), ss.get_length(splitter) + 1,
                std::back_inserter(raw_splitters));
          }
          return raw_splitters; 
        }
  };

  
  template <typename StringSet>
    StringLcpContainer<StringSet> choose_splitters(
        const StringSet& ss, 
        std::vector<typename StringSet::Char>& all_splitters,
        dsss::mpi::environment env = dsss::mpi::environment())
    {
      using Char = typename StringSet::Char;
      using String = typename StringSet::String;

      StringLcpContainer<StringSet> all_splitters_cont(std::move(all_splitters));
      dss_schimek::StringLcpPtr all_splitters_strptr = all_splitters_cont.make_string_lcp_ptr();
      const StringSet& all_splitters_set = all_splitters_strptr.active();

      insertion_sort(all_splitters_strptr, 0, 0);

      const size_t nr_splitters = std::min<std::size_t>(env.size() - 1, all_splitters_set.size());
      const size_t splitter_dist = all_splitters_set.size() / (nr_splitters + 1);

      std::vector<Char> raw_chosen_splitters;
      for (std::size_t i = 1; i <= nr_splitters; ++i) {
        const auto begin = all_splitters_set.begin();
        const String splitter = all_splitters_set[begin + i * splitter_dist];
        std::copy_n(ss.get_chars(splitter, 0), ss.get_length(splitter) + 1,
            std::back_inserter(raw_chosen_splitters));
      }
      return StringLcpContainer<StringSet>(std::move(raw_chosen_splitters));
    }

  template <typename StringSet>
    inline std::vector<size_t> compute_interval_sizes(const StringSet& ss, 
        const StringSet& splitters,
        dsss::mpi::environment env = dsss::mpi::environment())
    {
      std::vector<size_t> interval_sizes;
      interval_sizes.reserve(splitters.size());

      size_t nr_splitters = std::min<size_t>(env.size() - 1, ss.size());
      size_t splitter_dist = ss.size() / (nr_splitters + 1);
      size_t element_pos = 0;

      for (std::size_t i = 0; i < splitters.size(); ++i) {
        element_pos = (i + 1) * splitter_dist;

        while(element_pos > 0 && !dss_schimek::leq(
              ss.get_chars(ss[ss.begin() + element_pos], 0), 
              splitters.get_chars(splitters[splitters.begin() + i], 0))) 
        { --element_pos; }

        while (element_pos < ss.size() && dss_schimek::leq(
              ss.get_chars(ss[ss.begin() + element_pos], 0),
              splitters.get_chars(splitters[splitters.begin() + i], 0))) 
        { ++element_pos; }

        interval_sizes.emplace_back(element_pos);
      }
      interval_sizes.emplace_back(ss.size());
      for (std::size_t i = interval_sizes.size() - 1; i > 0; --i) {
        interval_sizes[i] -= interval_sizes[i - 1];
      }
      return interval_sizes;
    }

    static inline void print_interval_sizes(const std::vector<size_t>& sent_interval_sizes,
      const std::vector<size_t>& recv_interval_sizes,
      dsss::mpi::environment env = dsss::mpi::environment())
  {
    constexpr bool print_interval_details = true;
    if constexpr (print_interval_details) {
      for (std::int32_t rank = 0; rank < env.size(); ++rank) {
        if (env.rank() == rank) {
          std::size_t total_size = 0;
          std::cout << "### Sending interval sizes on PE " << rank << std::endl;
          for (const auto is : sent_interval_sizes) {
            total_size += is;
            std::cout << is << ", ";
          }
          std::cout << "Total size: " << total_size << std::endl;
        }
        env.barrier();
      }
      for (std::int32_t rank = 0; rank < env.size(); ++rank) {
        if (env.rank() == rank) {
          std::size_t total_size = 0;
          std::cout << "### Receiving interval sizes on PE " << rank << std::endl;
          for (const auto is : recv_interval_sizes) {
            total_size += is;
            std::cout << is << ", ";
          }
          std::cout << "Total size: " << total_size << std::endl;
        }
        env.barrier();
      }
      if (env.rank() == 0) { std::cout << std::endl; }
    }
  }

  template <typename StringLcpContainer>
    static inline std::vector<std::pair<size_t, size_t>> compute_ranges_and_set_lcp_at_start_of_range(
        StringLcpContainer& recv_string_cont,
        std::vector<size_t>& recv_interval_sizes,
        dsss::mpi::environment env = dsss::mpi::environment())
    {
      std::vector<std::pair<size_t, size_t>> ranges;
      for(size_t i = 0, offset = 0; i < env.size(); ++i)
      {
        if (recv_interval_sizes[i] == 0)
        {
          ranges.emplace_back(0, 0);
          continue;
        }
        *(recv_string_cont.lcp_array() + offset) = 0;
        ranges.emplace_back(offset, recv_interval_sizes[i]);
        offset += recv_interval_sizes[i];
      }

      if constexpr (debug)
        dss_schimek::mpi::execute_in_order([&] () {
            std::cout << "rank: " << env.rank() << " pairs:" << std::endl;
            for(size_t i = 0; i < env.size(); ++i)
            std::cout << i << " " << ranges[i].first << " " << ranges[i].second << std::endl;
            });

      return ranges;
    }
  template<size_t K>
    static inline StringLcpContainer<UCharLengthStringSet> merge(
        dss_schimek::StringLcpContainer<UCharLengthStringSet>&& recv_string_cont,
        const std::vector<std::pair<size_t, size_t>>& ranges,
        const size_t num_recv_elems) {

      std::vector<unsigned char*> strings(num_recv_elems);
      for (size_t i = 0; i < num_recv_elems; ++i)
        strings[i] = recv_string_cont[i].string;

      stringtools::LcpStringPtr lt_all_strings(strings.data(),
          recv_string_cont.lcp_array(),
          recv_string_cont.size());

      std::vector<unsigned char*> sorted_string(recv_string_cont.size());
      std::vector<size_t> sorted_lcp(recv_string_cont.size());
      bingmann::LcpStringLoserTree<K> loser_tree_(lt_all_strings, ranges.data());
      stringtools::LcpStringPtr out_(sorted_string.data(),
          sorted_lcp.data(),
          num_recv_elems);
      loser_tree_.writeElementsToStream(out_, num_recv_elems);

      StringLcpContainer<UCharLengthStringSet> sorted_string_cont;
      sorted_string_cont.set(std::move(recv_string_cont.raw_strings()));
      using String = typename UCharLengthStringSet::String;
      std::vector<String> tmp(num_recv_elems, String(0,0));
      for (size_t i = 0; i < num_recv_elems; ++i)
        tmp[i].string = sorted_string[i];
      sorted_string_cont.set(std::move(tmp));
      sorted_string_cont.set(std::move(sorted_lcp));
      return sorted_string_cont;
    }
  template<size_t K, typename StringSet>
    static inline StringLcpContainer<StringSet> merge(
        dss_schimek::StringLcpContainer<StringSet>&& recv_string_cont,
        const std::vector<std::pair<size_t, size_t>>& ranges,
        const size_t num_recv_elems) {

      stringtools::LcpStringPtr lt_all_strings(recv_string_cont.strings(),
          recv_string_cont.lcp_array(),
          recv_string_cont.size());

      std::vector<unsigned char*> sorted_string(recv_string_cont.size());
      std::vector<size_t> sorted_lcp(recv_string_cont.size());
      bingmann::LcpStringLoserTree<K> loser_tree_(lt_all_strings, ranges.data());
      stringtools::LcpStringPtr out_(sorted_string.data(),
          sorted_lcp.data(),
          num_recv_elems);
      loser_tree_.writeElementsToStream(out_, num_recv_elems);

      StringLcpContainer<StringSet> sorted_string_cont;
      sorted_string_cont.set(std::move(recv_string_cont.raw_strings()));
      sorted_string_cont.set(std::move(sorted_string));
      sorted_string_cont.set(std::move(sorted_lcp));
      return sorted_string_cont;
    }

  template<typename StringLcpContainer>
    static inline StringLcpContainer choose_merge(StringLcpContainer&& recv_string_cont,
        std::vector<std::pair<size_t, size_t>> ranges,
        size_t num_recv_elems,
        dsss::mpi::environment env = dsss::mpi::environment()) {

      switch (env.size()) {
        case 1 :  return merge<1>(std::move(recv_string_cont),
                      ranges,
                      num_recv_elems);
        case 2 : return merge<2>(std::move(recv_string_cont),
                     ranges,
                     num_recv_elems);
        case 4 : return merge<4>(std::move(recv_string_cont),
                     ranges,
                     num_recv_elems);
        case 8 : return merge<8>(std::move(recv_string_cont),
                     ranges,
                     num_recv_elems);
        case 16 : return merge<16>(std::move(recv_string_cont),
                      ranges,
                      num_recv_elems);
        case 32 : return merge<32>(std::move(recv_string_cont),
                      ranges,
                      num_recv_elems);
        case 64 : return merge<64>(std::move(recv_string_cont),
                      ranges,
                      num_recv_elems);
        case 128 : return merge<128>(std::move(recv_string_cont),
                       ranges,
                       num_recv_elems);
        case 264 : return merge<264>(std::move(recv_string_cont),
                       ranges,
                       num_recv_elems);
        case 512 : return merge<512>(std::move(recv_string_cont),
                       ranges,
                       num_recv_elems);
        default : std::abort();
      }
      return StringLcpContainer();
    }

  template<typename StringPtr, typename SampleSplittersPolicy>
    class DistributedMergeSort : private SampleSplittersPolicy
  {
    public:
      dss_schimek::StringLcpContainer<typename StringPtr::StringSet>
        sort(StringPtr& local_string_ptr,
            dss_schimek::StringLcpContainer<typename StringPtr::StringSet>&& local_string_container, 
            Timer& timer,
            dsss::mpi::environment env = dsss::mpi::environment()) {

          constexpr bool debug = false;

          using StringSet = typename StringPtr::StringSet;
          using Char = typename StringSet::Char;
          const StringSet& ss = local_string_ptr.active();
          std::size_t local_n = ss.size();
          
          // sort locally
          timer.start("sort locally");
          dss_schimek::multikey_quicksort(local_string_ptr, 0, 0);
          timer.end("sort locally");

          // There is only one PE, hence there is no need for distributed sorting 
          if (env.size() == 1)
            return dss_schimek::StringLcpContainer<StringSet>(std::move(local_string_container));

          timer.start("sample splitters");
          std::vector<Char> raw_splitters = SampleSplittersPolicy::sample_splitters(ss);
          timer.end("sample splitters");

          timer.start("allgather splitters");
          std::vector<Char> splitters =
            dss_schimek::mpi::allgather_strings(raw_splitters, env);
          timer.end("allgather splitters");

          timer.start("choose splitters");
          dss_schimek::StringLcpContainer chosen_splitters_cont = choose_splitters(ss, splitters);
          timer.end("choose splitters");


          const StringSet chosen_splitters_set(chosen_splitters_cont.strings(),
              chosen_splitters_cont.strings() + chosen_splitters_cont.size());

          timer.start("compute interval sizes");
          std::vector<std::size_t> interval_sizes = compute_interval_sizes(ss, chosen_splitters_set);
          std::vector<std::size_t> receiving_interval_sizes = dsss::mpi::alltoall(interval_sizes);
          timer.end("compute interval sizes");
          //print_interval_sizes(interval_sizes, receiving_interval_sizes);

          timer.start("all-to-all strings");
          dss_schimek::StringLcpContainer<StringSet> recv_string_cont = 
            dsss::mpi::alltoallv(local_string_container, interval_sizes);
          timer.end("all-to-all strings");

          std::cout << "rank: " << env.rank() << " received num chars: " << recv_string_cont.char_size() - recv_string_cont.size() << std::endl;

          size_t num_recv_elems = 
            std::accumulate(receiving_interval_sizes.begin(), receiving_interval_sizes.end(), 0);

          assert(num_recv_elems == recv_string_cont.size());

          timer.start("compute ranges");
          std::vector<std::pair<size_t, size_t>> ranges = 
            compute_ranges_and_set_lcp_at_start_of_range(recv_string_cont, receiving_interval_sizes);
          timer.end("compute ranges");


          timer.start("merge ranges");
          auto sorted_container = choose_merge(std::move(recv_string_cont), ranges, num_recv_elems);
          timer.end("merge ranges");
          return sorted_container;
        }
  };

}
