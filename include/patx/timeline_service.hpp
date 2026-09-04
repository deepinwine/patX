// Builds the prosecution timeline for a US case from stored documents and
// rejection data. Events reflect what actually exists in the file wrapper -
// no template is forced onto the case.
#pragma once

#include "patx/uspto_models.hpp"

#include <string>
#include <vector>

struct sqlite3;

namespace patx {

class UsptoRepository;

class TimelineService {
public:
    explicit TimelineService(UsptoRepository& repo);

    // ascending = chronological order, descending = newest first
    std::vector<TimelineEvent> BuildTimeline(int uspto_case_id, bool ascending = true);

    // Links applicant responses/amendments to the OA they answer (fills
    // related_document_id). Conservative: only links to the single most
    // recent unanswered OA before the response date.
    int LinkResponsesToOfficeActions(int uspto_case_id);

private:
    UsptoRepository& repo_;
};

} // namespace patx
