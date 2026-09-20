#include "patx/oa_parser.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

namespace patx {

namespace {

std::string Upper(const std::string& s) {
    std::string out;
    for (char c : s) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

std::vector<int> OaParser::ParseClaimList(const std::string& text) {
    std::vector<int> claims;
    std::regex range_re(R"((\d{1,3})\s*(?:-|–|to|through)\s*(\d{1,3}))");
    std::set<int> unique;

    // First replace ranges with their members, then pick up singles
    std::string remaining = text;
    auto begin = std::sregex_iterator(remaining.begin(), remaining.end(), range_re);
    auto end_it = std::sregex_iterator();
    size_t offset = 0;
    std::string consumed;
    for (auto it = begin; it != end_it; ++it) {
        int first = std::stoi((*it)[1].str());
        int last = std::stoi((*it)[2].str());
        if (last < first) std::swap(first, last);
        if (last - first > 100) continue;  // implausible
        for (int n = first; n <= last; n++) unique.insert(n);
        consumed += remaining.substr(offset, static_cast<size_t>(it->position()) - offset) + " ";
        offset = static_cast<size_t>(it->position() + it->length());
    }
    consumed += remaining.substr(offset);

    std::regex single_re(R"(\b(\d{1,3})\b)");
    auto s_begin = std::sregex_iterator(consumed.begin(), consumed.end(), single_re);
    for (auto it = s_begin; it != std::sregex_iterator(); ++it) {
        int n = std::stoi((*it)[1].str());
        if (n >= 1 && n <= 999) unique.insert(n);
    }

    claims.assign(unique.begin(), unique.end());
    return claims;
}

OaParseResult OaParser::Parse(const std::string& document_text) {
    OaParseResult result;
    const std::string upper = Upper(document_text);

    // ---- Office action detection ----
    bool nonfinal = upper.find("NON-FINAL REJECTION") != std::string::npos ||
                    upper.find("NONFINAL REJECTION") != std::string::npos;
    bool final_rej = upper.find("FINAL REJECTION") != std::string::npos &&
                     upper.find("NON-FINAL") == std::string::npos;
    bool restriction = upper.find("RESTRICTION REQUIREMENT") != std::string::npos;
    if (!nonfinal && !final_rej && !restriction) {
        if (upper.find("OFFICE ACTION") == std::string::npos) {
            result.is_office_action = false;
            return result;
        }
        nonfinal = true;  // generic office action header, assume non-final
    }
    result.is_office_action = true;
    result.oa_type = restriction ? "Restriction" : (final_rej ? "Final" : "Non-Final");
    result.confidence = 0.7;

    // ---- Mail date: "MAIL DATE" or first Mailed YYYY-MM-DD ----
    std::regex mail_re(R"(MAIL\s*DATE[^\d]{0,30}(\d{4}[-/]\d{1,2}[-/]\d{1,2}))",
                       std::regex::icase);
    std::smatch m;
    if (std::regex_search(document_text, m, mail_re)) {
        std::string date = m[1].str();
        std::replace(date.begin(), date.end(), '/', '-');
        result.mail_date = date;
    }

    // ---- Examiner ----
    std::regex examiner_re(R"(EXAMINER[,\s]+([A-Z][A-Za-z\.]+(?:\s+[A-Z][A-Za-z\.]+){0,3}))");
    if (std::regex_search(document_text, m, examiner_re)) {
        result.examiner_name = m[1].str();
    }

    // ---- Art unit ----
    std::regex art_re(R"(ART UNIT[:\s]+(\d{4}))", std::regex::icase);
    if (std::regex_search(document_text, m, art_re)) {
        result.art_unit = m[1].str();
    }

    // ---- Response period ----
    if (upper.find("SHORTENED STATUTORY PERIOD") != std::string::npos) {
        result.response_period = "Shortened statutory period (see OA for exact date)";
    } else if (upper.find("SIX (6) MONTHS") != std::string::npos || upper.find("6 MONTHS") != std::string::npos) {
        result.response_period = "6 months";
    } else if (upper.find("THREE (3) MONTHS") != std::string::npos || upper.find("3 MONTHS") != std::string::npos) {
        result.response_period = "3 months";
    }

    // ---- Statutory rejections ----
    struct StatutePattern {
        const char* statute;
        const char* type;
        const char* pattern;   // heading-like occurrence
    };
    const StatutePattern statutes[] = {
        {"35 U.S.C. 101", "subject matter eligibility",
         R"(REJECT\w*\s+(?:UNDER|BASED\s+ON)\s+35\s+U\.?S\.?C\.?\s*101)"},
        {"35 U.S.C. 102", "anticipation",
         R"((?:REJECT\w*\s+(?:UNDER|BASED\s+ON)|ANTICIPATED\s+(?:BY|UNDER))\s+35\s+U\.?S\.?C\.?\s*102)"},
        {"35 U.S.C. 103", "obviousness",
         R"((?:REJECT\w*\s+(?:UNDER|BASED\s+ON)|OBVIOUS\s+(?:OVER|IN\s+VIEW\s+OF))\s+35\s+U\.?S\.?C\.?\s*103)"},
        {"35 U.S.C. 112(a)", "written description / enablement",
         R"(REJECT\w*\s+(?:UNDER|BASED\s+ON)\s+35\s+U\.?S\.?C\.?\s*112\s*\(\s*a\s*\)|WRITTEN\s+DESCRIPTION)"},
        {"35 U.S.C. 112(b)", "indefiniteness",
         R"(REJECT\w*\s+(?:UNDER|BASED\s+ON)\s+35\s+U\.?S\.?C\.?\s*112\s*\(\s*b\s*\)|INDEFINITE)"},
        {"double patenting", "obviousness-type double patenting",
         R"(DOUBLE\s+PATENTING)"},
    };

    std::set<std::string> seen_statutes;
    for (const auto& sp : statutes) {
        std::regex re(sp.pattern);
        auto b = std::sregex_iterator(upper.begin(), upper.end(), re);
        auto e = std::sregex_iterator();
        if (b == e) continue;
        seen_statutes.insert(sp.statute);
        result.confidence = std::max(result.confidence, 0.75);

        OaRejection rejection;
        rejection.statute = sp.statute;
        rejection.rejection_type = sp.type;
        rejection.parse_confidence = 0.7;

        // Claims: look at the window right after the first match for
        // "claims 1, 3 and 5-8" style lists.
        size_t pos = static_cast<size_t>(b->position());
        size_t window_end = std::min(upper.size(), pos + 1500);
        std::string window = document_text.substr(pos, window_end - pos);
        std::regex claims_re(R"((?:claim|claims)\s*\(?(?:s\)?\s*(?:is|are)?)?\s*([0-9,\s\-–andto]{1,60}))",
                             std::regex::icase);
        std::smatch cm;
        if (std::regex_search(window, cm, claims_re)) {
            rejection.claim_numbers = cm[1].str();
            for (int n : ParseClaimList(cm[1].str())) {
                if (std::find(result.claims_rejected.begin(), result.claims_rejected.end(), n) ==
                    result.claims_rejected.end()) {
                    result.claims_rejected.push_back(n);
                }
            }
        }

        // Primary reference: "U.S. Pat. No. 5,123,456 to Smith" near the statute
        std::regex pat_ref(R"((?:U\.?S\.?\s*(?:Patent|Pat\.?)\s*(?:No\.?|Number)\s*[:#]?\s*([RE\d,]{3,12})\s*(?:to\s+([A-Z][A-Za-z]+))?))");
        std::smatch pm;
        if (std::regex_search(window, pm, pat_ref)) {
            rejection.primary_reference = "US " + pm[1].str() + (pm[2].matched ? " to " + pm[2].str() : "");
        } else {
            // In view of X and Y pattern
            std::regex view_of(R"(in\s+view\s+of\s+(.{5,120}?)\s+(?:and|further|where|to\s+one))",
                               std::regex::icase);
            if (std::regex_search(window, pm, view_of)) {
                rejection.reference_text = pm[1].str();
            }
        }
        result.rejections.push_back(std::move(rejection));
    }

    // ---- Allowed / objected claims ----
    std::regex allowed_re(R"(claim(?:s)?\s*([0-9,\s\-–andto]{1,60})\s*(?:is|are)\s+(?:ALLOWED|ALLOWABLE))",
                          std::regex::icase);
    for (auto it = std::sregex_iterator(document_text.begin(), document_text.end(), allowed_re);
         it != std::sregex_iterator(); ++it) {
        for (int n : ParseClaimList((*it)[1].str())) {
            if (std::find(result.claims_allowed.begin(), result.claims_allowed.end(), n) ==
                result.claims_allowed.end()) {
                result.claims_allowed.push_back(n);
            }
        }
    }
    std::regex objected_re(R"(claim(?:s)?\s*([0-9,\s\-–andto]{1,60})\s*(?:is|are)\s+OBJECTED)",
                           std::regex::icase);
    for (auto it = std::sregex_iterator(document_text.begin(), document_text.end(), objected_re);
         it != std::sregex_iterator(); ++it) {
        for (int n : ParseClaimList((*it)[1].str())) {
            if (std::find(result.claims_objected.begin(), result.claims_objected.end(), n) ==
                result.claims_objected.end()) {
                result.claims_objected.push_back(n);
            }
        }
    }

    // ---- Cited references (patent + NPL) ----
    std::set<std::string> refs;
    std::regex all_pat_refs(
        R"((?:U\.?S\.?\s*(?:Patent|Pat\.?)\s*(?:No\.?|Number)\s*[:#]?\s*([A-Z]{0,2}[\d,]{3,12})))");
    for (auto it = std::sregex_iterator(document_text.begin(), document_text.end(), all_pat_refs);
         it != std::sregex_iterator(); ++it) {
        refs.insert("US " + (*it)[1].str());
    }
    result.cited_patent_references.assign(refs.begin(), refs.end());

    return result;
}

} // namespace patx
