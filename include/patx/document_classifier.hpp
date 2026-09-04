// Classifies USPTO file-wrapper documents into a stable taxonomy using the
// official document code + description + title, not a single string match.
// Raw code/description are always preserved by the repository.
#pragma once

#include "patx/uspto_models.hpp"

#include <string>

namespace patx {

class DocumentClassifier {
public:
    struct Result {
        UsptoDocumentCategory category = UsptoDocumentCategory::Unknown;
        FilingParty party = FilingParty::Unknown;
        double confidence = 0.5;
    };

    static Result Classify(const std::string& document_code,
                           const std::string& description,
                           const std::string& title = "");
};

} // namespace patx
