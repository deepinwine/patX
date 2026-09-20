#include "patx/claim_parser.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace patx {

namespace {

std::string CleanWhitespace(const std::string& input) {
    // Drop form feeds and page furniture, collapse all whitespace runs to
    // single spaces, trim. Page headers/footers from PDF extraction (e.g.
    // "AMENDMENT - SHEET 1 OF 5") are filtered line by line.
    std::string cleaned;
    std::string line;
    auto flush = [&]() {
        std::string upper;
        upper.reserve(line.size());
        for (char c : line) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        bool furniture = upper.find("SHEET ") != std::string::npos && upper.find(" OF ") != std::string::npos;
        furniture = furniture || upper.find("PAGE ") != std::string::npos ||
                    upper.rfind("CERTIFICATION", 0) == 0 || upper.rfind("PTO/SB/", 0) == 0;
        if (!furniture && !line.empty()) {
            if (!cleaned.empty()) cleaned += ' ';
            cleaned += line;
        }
        line.clear();
    };
    for (char c : input) {
        if (c == '\f') { flush(); continue; }
        if (c == '\n' || c == '\r') { flush(); continue; }
        if (c == '\t') c = ' ';
        if (c == ' ' && line.empty()) continue;
        line += c;
    }
    flush();

    std::string collapsed;
    bool prev_space = true;
    for (char c : cleaned) {
        if (c == ' ') {
            if (!prev_space) collapsed += ' ';
            prev_space = true;
        } else {
            collapsed += c;
            prev_space = false;
        }
    }
    return collapsed;
}

// Locates the slice of text containing the claim listing.
struct ListingSlice {
    std::string text;
    bool found = false;
};

ListingSlice FindClaimsSection(const std::string& text) {
    ListingSlice slice;
    std::string upper;
    upper.reserve(text.size());
    for (char c : text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    size_t claims_start = std::string::npos;
    size_t claims_end = text.size();

    // 1. Amendment "IN THE CLAIMS" section: ends at the next section header
    size_t in_claims = upper.find("IN THE CLAIMS");
    if (in_claims != std::string::npos) {
        claims_start = in_claims;
        for (const char* terminator : {"IN THE SPECIFICATION", "IN THE DRAWINGS",
                                       "REMARKS", "ARGUMENT", "ASSEMBLY", "EXECUTION", "DECLARATION"}) {
            size_t t = upper.find(terminator, in_claims + 13);
            if (t != std::string::npos && t < claims_end) claims_end = t;
        }
    }

    // 2. Specification headings
    if (claims_start == std::string::npos) {
        size_t claims_colon = upper.find("CLAIMS:");
        size_t claims_word = upper.find("WHAT IS CLAIMED IS");
        size_t heading = std::min(claims_colon == std::string::npos ? std::string::npos : claims_colon,
                                  claims_word == std::string::npos ? std::string::npos : claims_word);
        if (heading != std::string::npos) claims_start = heading;
    }

    // 3. Fallback: first "1." followed by a "2." nearby (numbered list in
    // remarks would also match, so require the follow-up claim)
    if (claims_start == std::string::npos) {
        std::regex first_claim(R"((?:^|\s)1[.)]\s)");
        std::regex second_claim(R"((?:^|\s)2[.)]\s)");
        auto begin = std::sregex_iterator(text.begin(), text.end(), first_claim);
        auto end_it = std::sregex_iterator();
        for (auto it = begin; it != end_it; ++it) {
            size_t pos = static_cast<size_t>(it->position());
            if (std::regex_search(text.begin() + static_cast<long>(pos),
                                  text.begin() + static_cast<long>(std::min(pos + 20000, text.size())),
                                  second_claim)) {
                claims_start = pos;
                break;
            }
        }
    }

    if (claims_start == std::string::npos) return slice;
    slice.text = text.substr(claims_start, claims_end - claims_start);
    slice.found = true;
    return slice;
}

} // namespace

ClaimStatus ClaimParser::ParseStatusIdentifier(const std::string& bracket_text) {
    std::string upper;
    for (char c : bracket_text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper.find("CURRENTLY AMENDED") != std::string::npos) return ClaimStatus::CurrentlyAmended;
    if (upper.find("PREVIOUSLY PRESENTED") != std::string::npos) return ClaimStatus::PreviouslyPresented;
    if (upper.find("CANCEL") != std::string::npos || upper.find("CANCELL") != std::string::npos)
        return ClaimStatus::Canceled;
    if (upper.find("WITHDRAWN") != std::string::npos) return ClaimStatus::Withdrawn;
    if (upper.find("NOT ENTERED") != std::string::npos) return ClaimStatus::NotEntered;
    if (upper.find("NEW") != std::string::npos || upper.find("ADDED") != std::string::npos)
        return ClaimStatus::New;
    if (upper.find("ORIGINAL") != std::string::npos) return ClaimStatus::Original;
    if (upper.find("ALLOWED") != std::string::npos) return ClaimStatus::Allowed;
    return ClaimStatus::Unknown;
}

std::vector<int> ClaimParser::ParseDependency(const std::string& claim_text) {
    std::vector<int> parents;
    // "of claim 1", "of claims 1-3", "of any one of claims 1 to 3",
    // "according to claim 7", "claim(s) 2".
    // NOTE: the [^\d]{0,12} bridge (instead of optional literal "(s)") is
    // deliberate: libc++'s std::regex mishandles backtracking through
    // optional groups here.
    std::regex range_re(
        R"((?:of|under|according\s+to)\s+((?:any\s+one\s+of\s+)?(?:the\s+)?claims?[^\d]{0,12}(\d{1,3})(?:\s*(?:-|to|through)\s*(\d{1,3}))?))",
        std::regex::icase);
    auto begin = std::sregex_iterator(claim_text.begin(), claim_text.end(), range_re);
    auto end_it = std::sregex_iterator();
    for (auto it = begin; it != end_it; ++it) {
        int first = std::stoi((*it)[2].str());
        std::string second_str = (*it)[3].str();
        int last = second_str.empty() ? first : std::stoi(second_str);
        if (last < first) std::swap(first, last);
        if (last - first > 50) continue;  // implausible range, likely a false hit
        for (int n = first; n <= last; n++) {
            if (std::find(parents.begin(), parents.end(), n) == parents.end()) {
                parents.push_back(n);
            }
        }
        // The lead-in phrase defines the dependency - only the first match
        // counts ("The device of claim 1, wherein the sensor of claim 3..." -
        // claim 3's mention inside the body does not create dependency).
        break;
    }
    std::sort(parents.begin(), parents.end());
    return parents;
}

ClaimParseResult ClaimParser::Parse(const std::string& document_text) {
    ClaimParseResult result;

    ListingSlice slice = FindClaimsSection(document_text);
    if (!slice.found) {
        result.error = "no claim listing found in document text";
        return result;
    }

    std::string text = CleanWhitespace(slice.text);

    // Collect claim-start matches in document order
    struct MatchInfo {
        int number = 0;
        size_t start = 0;     // where "12." begins
        size_t body_start = 0;// just past "12. "
    };
    std::vector<MatchInfo> matches;

    std::regex claim_start(R"((?:^|\s)(\d{1,3})[.)]\s)");
    auto begin = std::sregex_iterator(text.begin(), text.end(), claim_start);
    auto end_it = std::sregex_iterator();
    for (auto it = begin; it != end_it; ++it) {
        int number = std::stoi((*it)[1].str());
        if (number == 0) continue;
        // Claims ascend one by one; anything else (e.g. a year "2005.") is
        // body text, not a new claim.
        if (!matches.empty()) {
            if (number != matches.back().number + 1) continue;
        } else {
            if (number != 1) continue;
        }
        MatchInfo m;
        m.number = number;
        size_t prefix = std::isspace(static_cast<unsigned char>(it->str()[0])) ? 1 : 0;
        m.start = static_cast<size_t>(it->position()) + prefix;
        m.body_start = static_cast<size_t>(it->position()) + it->length();
        matches.push_back(m);
    }

    if (matches.empty()) {
        result.error = "claim section found but no numbered claims detected";
        return result;
    }
    result.found_listing = true;

    int status_bracket_count = 0;
    for (size_t i = 0; i < matches.size(); i++) {
        const MatchInfo& m = matches[i];
        size_t body_end = (i + 1 < matches.size()) ? matches[i + 1].start : text.size();
        std::string body = text.substr(m.body_start, body_end - m.body_start);

        // Strip a trailing truncation artifact
        while (!body.empty() && (body.back() == ' ' || body.back() == ';')) body.pop_back();

        Claim claim;
        claim.claim_number = m.number;
        claim.status = ClaimStatus::Original;   // unmarked claims in a listing are original

        // Status identifier "(Currently Amended)" right after the number
        size_t rel = 0;
        while (rel < body.size() && body[rel] == ' ') rel++;
        if (rel < body.size() && body[rel] == '(') {
            size_t close = body.find(')', rel);
            if (close != std::string::npos && close - rel < 60) {
                std::string bracket = body.substr(rel + 1, close - rel - 1);
                ClaimStatus status = ParseStatusIdentifier(bracket);
                if (status != ClaimStatus::Unknown) {
                    claim.status = status;
                    claim.claim_text_marked = body.substr(rel);   // keep the marker form
                    body = body.substr(close + 1);
                    while (!body.empty() && body.front() == ' ') body.erase(body.begin());
                    status_bracket_count++;
                }
            }
        }
        claim.claim_text_clean = body;

        if (claim.status == ClaimStatus::Canceled) {
            // "(Canceled)" claims normally carry no further text
            claim.claim_text_clean = "";
            claim.claim_text_marked = "";
            claim.is_independent = false;
        } else {
            claim.parent_claim_numbers = ParseDependency(body);
            claim.is_independent = claim.parent_claim_numbers.empty();
        }
        claim.parse_confidence = claim.status == ClaimStatus::Unknown ? 0.5 : 1.0;
        result.claims.push_back(std::move(claim));
    }

    // Confidence: share of claims that carried an explicit status identifier.
    // A clean 1.121 amendment listing has brackets on every claim.
    result.confidence = static_cast<double>(status_bracket_count) /
                        static_cast<double>(result.claims.size());
    return result;
}

} // namespace patx
