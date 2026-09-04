// Word-level claim comparison, aligned by claim number (never a whole-file
// diff). Produces per-token insert/delete/unchanged runs for the diff viewer.
#pragma once

#include "patx/uspto_models.hpp"

#include <string>
#include <vector>

namespace patx {

enum class DiffOp { Unchanged, Added, Removed };

struct DiffSegment {
    DiffOp op = DiffOp::Unchanged;
    std::string text;    // includes its trailing space when it had one
};

struct ClaimDiff {
    int claim_number = 0;
    bool both_present = false;
    bool added_claim = false;      // not in previous version
    bool removed_claim = false;    // not in current version (canceled or deleted)
    ClaimStatus previous_status = ClaimStatus::Unknown;
    ClaimStatus current_status = ClaimStatus::Unknown;
    std::vector<DiffSegment> segments;
    int added_words = 0;
    int removed_words = 0;

    bool HasChanges() const { return added_words > 0 || removed_words > 0 ||
                                     added_claim || removed_claim ||
                                     previous_status != current_status; }
};

class ClaimDiffEngine {
public:
    // Tokenizes on word boundaries, runs an LCS-based diff, returns runs.
    static std::vector<DiffSegment> DiffText(const std::string& previous,
                                             const std::string& current,
                                             int* added_words = nullptr,
                                             int* removed_words = nullptr);

    // Compares two claim snapshots claim-by-claim. Claims present on only one
    // side are reported as added/removed. Canceled claims show as removed.
    static std::vector<ClaimDiff> DiffClaimSets(const std::vector<Claim>& previous,
                                                const std::vector<Claim>& current);
};

} // namespace patx
