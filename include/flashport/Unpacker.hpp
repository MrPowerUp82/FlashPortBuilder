#pragma once
#include "flashport/SWFDocument.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flashport {

struct UnpackResult {
    std::string method;              // human readable description of what was undone
    std::vector<std::uint8_t> swf;   // recovered inner SWF (FWS/CWS bytes)
};

// Try every known loader/packer scheme on `doc`:
//  1. plain SWF stored directly in DefineBinaryData;
//  2. Alchemy "XOR merge" loaders: a key array is built by an Alchemy FSM (C code),
//     payload = (binA XOR key[j], j cycling 0..keyLen) ++ binB, then Loader.loadBytes;
//  3. simple repeating-XOR payloads with a 1-3 byte key (recovered from the signature).
// Returns the first candidate that decodes as a structurally valid SWF.
std::optional<UnpackResult> unpackLoader(const SWFDocument& doc);

} // namespace flashport
