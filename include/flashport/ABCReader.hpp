#pragma once
#include "flashport/SWFReader.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace flashport {

class ABCReader {
public:
    ABCSummary analyze(const std::vector<std::uint8_t>& abc, const std::string& name) const;
};

} // namespace flashport
