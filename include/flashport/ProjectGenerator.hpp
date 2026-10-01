#pragma once
#include "flashport/SWFReader.hpp"
#include <string>

namespace flashport {

class ProjectGenerator {
public:
    void generate(const SWFSummary& summary,
                  const std::string& inputSwf,
                  const std::string& outputDir,
                  const std::string& runtimeDir) const;
};

} // namespace flashport
