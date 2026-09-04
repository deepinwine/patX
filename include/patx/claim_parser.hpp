// Parses US claim listings (37 CFR 1.121 status identifiers) out of amendment
// or specification text. The parser never throws on malformed input - it
// degrades gracefully and reports a confidence value instead.
#pragma once

#include "patx/uspto_models.hpp"

#include <string>
#include <vector>

namespace patx {

struct ClaimParseResult {
    std::vector<Claim> claims;
    double confidence = 0.0;      // 1.0 = clean listing with status identifiers
    bool found_listing = false;   // false when no claims section was detected
    std::string error;            // non-empty when found_listing is false
};

class ClaimParser {
public:
    // Extracts the claim listing from a full document text. When the text is
    // an amendment, prefers the "IN THE CLAIMS" section; falls back to the
    // first contiguous numbered claim sequence.
    static ClaimParseResult Parse(const std::string& document_text);

    // Dependency extraction: finds "of claim 1", "of claims 2-4",
    // "of any one of claims 1 to 3" etc. Returns claim numbers; empty for
    // independent claims. Failure never affects claim text.
    static std::vector<int> ParseDependency(const std::string& claim_text);

    // Normalizes a status identifier string like "currently amended" to the
    // enum; Unknown when unrecognized.
    static ClaimStatus ParseStatusIdentifier(const std::string& bracket_text);
};

} // namespace patx
