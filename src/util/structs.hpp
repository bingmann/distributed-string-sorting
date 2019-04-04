#pragma once
#include <cstddef>
#include <vector>
#include <iostream>
#include <numeric>
#include <algorithm>
#include "mpi/synchron.hpp"

struct StringIndexPEIndex {
  size_t stringIndex;
  size_t PEIndex;
  StringIndexPEIndex(const size_t stringIndex, const size_t PEIndex) 
    : stringIndex(stringIndex), PEIndex(PEIndex) {}
};

std::ostream& operator<<(std::ostream& stream, const StringIndexPEIndex& indices) {
  return stream << "[" << indices.stringIndex << ", " << indices.PEIndex << "]";
}

template<typename DataType>
inline std::vector<DataType> flatten(const std::vector<std::vector<DataType>>& dataToFlatten, const size_t totalSumElements) {
  std::vector<DataType> flattenedData;
  flattenedData.reserve(totalSumElements);
  for (const auto& curVec : dataToFlatten) {
    std::copy(curVec.begin(), curVec.end(), std::back_inserter(flattenedData));
  }
  return flattenedData;
}

template<typename DataType>
inline std::vector<DataType> flatten(const std::vector<std::vector<DataType>>& dataToFlatten) {
  size_t totalSumElements = 0;
  for (const auto& curVec : dataToFlatten)
    totalSumElements += curVec.size();

  return flatten(dataToFlatten, totalSumElements);
}

template <typename StringSet, typename Iterator>
void reorder(StringSet ss, Iterator begin, Iterator end) {
  using String = typename StringSet::String;

  dss_schimek::mpi::environment env;
  std::vector<String> reorderedStrings;
  std::vector<size_t> numberInStringSet(env.size(), 0);
  std::vector<size_t> startIndexInStringSet;
  std::vector<size_t> smallestIndexInPermutation(env.size(), std::numeric_limits<size_t>::max());

  reorderedStrings.reserve(end - begin);
  for (Iterator curIt = begin; curIt != end; ++curIt) {
    const auto& indices = *curIt;
    numberInStringSet[indices.PEIndex]++;
    size_t& curSmallestIndex = smallestIndexInPermutation[indices.PEIndex]; 
    curSmallestIndex = std::min(curSmallestIndex, indices.stringIndex);
  }
  startIndexInStringSet.push_back(0);
  std::partial_sum(numberInStringSet.begin(), numberInStringSet.end() - 1, std::back_inserter(startIndexInStringSet));

  //dss_schimek::mpi::execute_in_order([&]() {
  //    std::cout << "rank " << dss_schimek::mpi::environment().rank() << std::endl;
  //    for(Iterator curIt = begin; curIt != end; ++curIt) {
  //    const auto& indices = *curIt;
  //    const size_t stringOffset = startIndexInStringSet[indices.PEIndex];
  //    String str = ss[ss.begin() + stringOffset + indices.stringIndex - smallestIndexInPermutation[indices.PEIndex]];
  //    std::cout << *curIt << std::endl;
  //    std::cout << "stringOffset: " << stringOffset << " smallestIndexInPermutation " << smallestIndexInPermutation[indices.PEIndex] << std::endl;
  //    std::cout << ss.get_chars(str, 0) << std::endl;
  //    reorderedStrings.push_back(str);
  //    }
  //    });
  for(Iterator curIt = begin; curIt != end; ++curIt) {
    const auto& indices = *curIt;
    const size_t stringOffset = startIndexInStringSet[indices.PEIndex];
    String str = ss[ss.begin() + stringOffset + indices.stringIndex - smallestIndexInPermutation[indices.PEIndex]];
    reorderedStrings.push_back(str);
  }
  for (size_t i = 0; i < ss.size(); ++i)
    ss[ss.begin() + i] = reorderedStrings[i];
}
