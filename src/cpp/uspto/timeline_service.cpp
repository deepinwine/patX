#include "patx/timeline_service.hpp"
#include "patx/uspto_repository.hpp"

#include <algorithm>

namespace patx {

namespace {

bool IsOfficeAction(UsptoDocumentCategory c) {
    return c == UsptoDocumentCategory::NonFinalOfficeAction ||
           c == UsptoDocumentCategory::FinalOfficeAction ||
           c == UsptoDocumentCategory::RestrictionRequirement ||
           c == UsptoDocumentCategory::AdvisoryAction;
}

bool IsResponse(UsptoDocumentCategory c) {
    return c == UsptoDocumentCategory::ResponseToOfficeAction ||
           c == UsptoDocumentCategory::Amendment ||
           c == UsptoDocumentCategory::ClaimsAmendment ||
           c == UsptoDocumentCategory::AfterFinalAmendment ||
           c == UsptoDocumentCategory::SpecificationAmendment ||
           c == UsptoDocumentCategory::DrawingAmendment ||
           c == UsptoDocumentCategory::RCE;
}

std::string JoinStatutes(const std::vector<OaRejection>& rejections) {
    std::string summary;
    for (const auto& r : rejections) {
        if (!summary.empty()) summary += ", ";
        summary += r.statute;
        if (!r.claim_numbers.empty()) summary += " (" + r.claim_numbers + ")";
    }
    return summary;
}

} // namespace

TimelineService::TimelineService(UsptoRepository& repo) : repo_(repo) {}

std::vector<TimelineEvent> TimelineService::BuildTimeline(int uspto_case_id, bool ascending) {
    std::vector<TimelineEvent> events;
    auto docs = repo_.GetDocumentsForCaseOrdered(uspto_case_id, true);

    for (const auto& doc : docs) {
        TimelineEvent event;
        event.date = doc.mail_date.empty() ? doc.filing_date : doc.mail_date;
        event.event_category = doc.document_category.empty()
                                   ? "Unclassified" : doc.document_category;
        event.document_description = doc.document_description.empty()
                                         ? doc.document_code : doc.document_description;
        event.party = doc.party;
        event.document_id = doc.id;

        if (doc.category == UsptoDocumentCategory::NonFinalOfficeAction ||
            doc.category == UsptoDocumentCategory::FinalOfficeAction) {
            event.oa_type = doc.category == UsptoDocumentCategory::FinalOfficeAction
                                ? "Final" : "Non-Final";
            auto rejections = repo_.GetRejectionsForDocument(doc.id);
            event.rejection_summary = JoinStatutes(rejections);
            if (!rejections.empty()) {
                event.claims_affected = rejections.front().claim_numbers;
            }
        }
        events.push_back(std::move(event));
    }

    // Filing date anchors the timeline when the wrapper has no documents yet
    auto case_row = repo_.GetCaseById(uspto_case_id);
    if (events.empty() && !case_row.filing_date.empty()) {
        TimelineEvent filed;
        filed.date = case_row.filing_date;
        filed.event_category = "Application Filed";
        filed.document_description = "Application filed with USPTO";
        filed.party = FilingParty::Applicant;
        events.push_back(std::move(filed));
    }

    std::sort(events.begin(), events.end(), [ascending](const TimelineEvent& a, const TimelineEvent& b) {
        if (a.date != b.date) return ascending ? a.date < b.date : a.date > b.date;
        return ascending ? a.document_id < b.document_id : a.document_id > b.document_id;
    });
    return events;
}

int TimelineService::LinkResponsesToOfficeActions(int uspto_case_id) {
    auto docs = repo_.GetDocumentsForCaseOrdered(uspto_case_id, true);
    int linked = 0;
    int last_unanswered_oa = 0;

    for (const auto& doc : docs) {
        if (IsOfficeAction(doc.category)) {
            if (last_unanswered_oa == 0) last_unanswered_oa = doc.id;
            continue;
        }
        if (IsResponse(doc.category) && last_unanswered_oa != 0 && doc.related_document_id == 0) {
            if (repo_.LinkDocuments(doc.id, last_unanswered_oa)) {
                linked++;
            }
            // This OA has now been answered; the next OA opens a new round.
            // A restriction requirement followed by an election response then
            // an OA keeps working naturally in date order.
            last_unanswered_oa = 0;
        }
    }
    return linked;
}

} // namespace patx
