#include "patx/claim_diff.hpp"

#include <algorithm>
#include <map>

namespace patx {

namespace {

std::vector<std::string> Tokenize(const std::string& text) {
    // Split into words, keeping punctuation attached (typical for claim text
    // where "element," vs "element" matters).
    std::vector<std::string> tokens;
    std::string current;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

} // namespace

std::vector<DiffSegment> ClaimDiffEngine::DiffText(const std::string& previous,
                                                   const std::string& current,
                                                   int* added_words, int* removed_words) {
    std::vector<DiffSegment> segments;
    if (added_words) *added_words = 0;
    if (removed_words) *removed_words = 0;
    if (previous.empty() && current.empty()) return segments;

    const auto a = Tokenize(previous);
    const auto b = Tokenize(current);
    const size_t n = a.size(), m = b.size();

    // Guard against pathological sizes (LCS table is (n+1)x(m+1))
    if (n * m > 40'000'000ull) {
        // Too large for a matrix: fall back to a coarse whole-block diff so
        // the UI stays responsive instead of exhausting memory.
        if (!previous.empty()) {
            segments.push_back({DiffOp::Removed, previous});
            if (removed_words) *removed_words = static_cast<int>(n);
        }
        if (!current.empty()) {
            segments.push_back({DiffOp::Added, current});
            if (added_words) *added_words = static_cast<int>(m);
        }
        return segments;
    }

    // LCS length table
    std::vector<std::vector<uint32_t>> lcs(n + 1, std::vector<uint32_t>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1
                                     : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }

    // Walk the table, emitting runs
    auto push = [&segments](DiffOp op, const std::string& token) {
        if (!segments.empty() && segments.back().op == op) {
            segments.back().text += (segments.back().text.empty() ? "" : " ") + token;
        } else {
            segments.push_back({op, token});
        }
    };

    size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            push(DiffOp::Unchanged, a[i]);
            i++; j++;
        } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
            push(DiffOp::Removed, a[i]);
            if (removed_words) (*removed_words)++;
            i++;
        } else {
            push(DiffOp::Added, b[j]);
            if (added_words) (*added_words)++;
            j++;
        }
    }
    while (i < n) {
        push(DiffOp::Removed, a[i]);
        if (removed_words) (*removed_words)++;
        i++;
    }
    while (j < m) {
        push(DiffOp::Added, b[j]);
        if (added_words) (*added_words)++;
        j++;
    }
    return segments;
}

std::vector<ClaimDiff> ClaimDiffEngine::DiffClaimSets(const std::vector<Claim>& previous,
                                                      const std::vector<Claim>& current) {
    std::map<int, const Claim*> prev_map;
    for (const auto& c : previous) prev_map[c.claim_number] = &c;
    std::map<int, const Claim*> cur_map;
    for (const auto& c : current) cur_map[c.claim_number] = &c;

    std::map<int, bool> all;
    for (const auto& [num, _] : prev_map) all[num] = true;
    for (const auto& [num, _] : cur_map) all[num] = true;

    std::vector<ClaimDiff> diffs;
    for (const auto& [num, _] : all) {
        ClaimDiff d;
        d.claim_number = num;

        const Claim* p = prev_map.count(num) ? prev_map[num] : nullptr;
        const Claim* c = cur_map.count(num) ? cur_map[num] : nullptr;

        if (p) d.previous_status = p->status;
        if (c) d.current_status = c->status;

        if (p && c) {
            d.both_present = true;
            // A canceled claim has no text; represent the cancellation as the
            // removal of the previous text so the viewer shows "CANCELED".
            d.segments = DiffText(p->claim_text_clean, c->claim_text_clean,
                                  &d.added_words, &d.removed_words);
        } else if (c && !p) {
            d.added_claim = true;
            d.segments = DiffText("", c->claim_text_clean, &d.added_words, &d.removed_words);
        } else if (p && !c) {
            d.removed_claim = true;
            d.segments = DiffText(p->claim_text_clean, "", &d.added_words, &d.removed_words);
        }
        diffs.push_back(std::move(d));
    }
    return diffs;
}

} // namespace patx
