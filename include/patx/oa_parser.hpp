// Deterministic Office Action rejection parser. Regex + heading based; no
// external model calls. Output is always marked with a confidence value so
// low-confidence extractions can be flagged "Needs Review" in the UI.
#pragma once

#include "patx/uspto_models.hpp"

#include <string>
#include <vector>

namespace patx {

struct OaParseResult {
    bool is_office_action = false;
    std::string oa_type;              // "Non-Final" / "Final" / "Restriction" / ...
    std::string mail_date;
    std::string examiner_name;
    std::string art_unit;
    std::string response_period;      // e.g. "3 months" / "SHORTENED STATUTORY PERIOD"
    std::vector<int> claims_rejected;
    std::vector<int> claims_allowed;
    std::vector<int> claims_objected;
    std::vector<OaRejection> rejections;
    std::vector<std::string> cited_patent_references;
    std::vector<std::string> cited_npl_references;
    double confidence = 0.0;
};

class OaParser {
public:
    static OaParseResult Parse(const std::string& document_text);

    // "1, 3, 5-8" -> {1,3,5,6,7,8}; tolerant of "claim(s)" prefixes.
    static std::vector<int> ParseClaimList(const std::string& text);
};

} // namespace patx
