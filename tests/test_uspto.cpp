// USPTO layer tests, all offline against checked-in public fixtures:
// JSON parsing, classification, claim parsing/diff, OA parsing, incremental
// dedup. Live API tests are intentionally NOT here - they run only when
// USPTO_API_KEY is set (see test_uspto_live.cpp gating note in README).
#include "test_registry.hpp"

#include "database.hpp"
#include "patx/claim_diff.hpp"
#include "patx/claim_parser.hpp"
#include "patx/document_classifier.hpp"
#include "patx/oa_parser.hpp"
#include "patx/uspto_client.hpp"
#include "patx/uspto_repository.hpp"

#include <filesystem>
#include <fstream>

using namespace testutil;

namespace {

std::string LoadFixture(const std::string& relative) {
    std::string path = FixturePath(relative);
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) Fail("fixture not found: " + path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

// ---------------- JSON parsing against ODP-shaped fixtures ----------------

TEST(uspto_parse_application_fixture) {
    std::string body = LoadFixture("uspto/application_17248024.json");
    patx::UsptoCase c;
    std::string error;
    CHECK(patx::UsptoClient::ParseApplicationBody(body, c, error));
    CHECK_STR_EQ(error, "");
    CHECK_STR_EQ(c.application_status, "Patented Case");
    CHECK_STR_EQ(c.filing_date, "2021-01-05");
    CHECK_STR_EQ(c.grant_date, "2023-05-09");
    CHECK_STR_EQ(c.patent_number, "11646472");
    CHECK_STR_EQ(c.examiner_name, "DOVE, TRACY MAE");
    CHECK_STR_EQ(c.art_unit, "1727");
    CHECK_STR_EQ(c.technology_center, "17");
    CHECK_STR_EQ(c.title, "MAKING LITHIUM METAL - SEAWATER BATTERY CELLS HAVING PROTECTED LITHIUM ELECTRODES");
    CHECK_STR_EQ(c.first_named_inventor, "Steven J. Visco");
    CHECK_STR_EQ(c.entity_status, "Regular Undiscounted");
    CHECK_STR_EQ(c.publication_number, "US20210210819A1");
}

TEST(uspto_parse_documents_fixture) {
    std::string body = LoadFixture("uspto/documents_17248024.json");
    std::vector<patx::UsptoDocument> docs;
    std::string error;
    CHECK(patx::UsptoClient::ParseDocumentsBody(body, docs, error));
    CHECK(!docs.empty());

    // Every parsed doc has the dedup key and a classified category
    bool found_petdec = false, found_wfee = false;
    for (const auto& d : docs) {
        CHECK(!d.document_identifier.empty());
        CHECK(d.category != patx::UsptoDocumentCategory::Unknown || d.parse_confidence < 0.5);
        CHECK(!d.raw_document_code.empty());
        if (d.document_code == "PETDEC") {
            found_petdec = true;
            CHECK_STR_EQ(d.filing_date, "2023-10-03");
            CHECK(!d.file_download_uri.empty());
            CHECK_STR_EQ(d.mime_type, "PDF");
            CHECK(d.xml_download_uri.find("xmlarchive") != std::string::npos);
        }
        if (d.document_code == "WFEE") found_wfee = true;
    }
    CHECK(found_petdec);
    CHECK(found_wfee);
}

TEST(uspto_normalize_application_number) {
    CHECK_STR_EQ(patx::UsptoClient::NormalizeApplicationNumber("17/248024"), "17248024");
    CHECK_STR_EQ(patx::UsptoClient::NormalizeApplicationNumber("US 17248024"), "17248024");
    CHECK_STR_EQ(patx::UsptoClient::NormalizeApplicationNumber("us17248024"), "17248024");
    CHECK_STR_EQ(patx::UsptoClient::NormalizeApplicationNumber("17248024"), "17248024");
}

// ---------------- Document classification ----------------

TEST(uspto_document_classifier_codes) {
    using C = patx::UsptoDocumentCategory;
    auto check = [](const char* code, const char* desc, C expected, patx::FilingParty party) {
        auto r = patx::DocumentClassifier::Classify(code, desc);
        CHECK_EQ(static_cast<int>(r.category), static_cast<int>(expected));
        CHECK_EQ(static_cast<int>(r.party), static_cast<int>(party));
    };
    check("CTNF", "Non-Final Office Action", C::NonFinalOfficeAction, patx::FilingParty::Examiner);
    check("CTFR", "Final Office Action", C::FinalOfficeAction, patx::FilingParty::Examiner);
    check("NOAL", "Notice of Allowance", C::NoticeOfAllowance, patx::FilingParty::Examiner);
    check("IDS", "Information Disclosure Statement", C::IDS, patx::FilingParty::Applicant);
    check("RCE", "", C::RCE, patx::FilingParty::Applicant);
    check("CTC", "Restriction Requirement", C::RestrictionRequirement, patx::FilingParty::Examiner);
    check("", "Amendment in response to Office Action", C::Amendment, patx::FilingParty::Applicant);
    check("WFEE", "Fee Worksheet (SB06)", C::FeeDocument, patx::FilingParty::Administrative);
    // Unknown codes fall through to description-based matching
    check("ZZZZ", "Mail Non-Final Office Action", C::NonFinalOfficeAction, patx::FilingParty::Examiner);
}

// ---------------- Claim parser ----------------

TEST(uspto_claim_parser_37cfr121_listing) {
    // Shape mirrors an amendment's IN THE CLAIMS section
    std::string amendment =
        "AMENDMENT IN THE CLAIMS:\n"
        "IN THE CLAIMS\n"
        "1. (Currently Amended) A lithium battery cell comprising a protected "
        "lithium electrode and an electrolyte, wherein the electrolyte comprises "
        "a nonaqueous solvent.\n"
        "2. (New) The cell of claim 1, wherein the solvent is organic.\n"
        "3. (Canceled)\n"
        "4. (Previously Presented) The cell of any one of claims 1-2, wherein "
        "the electrode is solid.\n"
        "5. (Originally Present) A method of making the cell of claim 1.\n"
        "REMARKS\n"
        "Applicant respectfully traverses...";

    auto result = patx::ClaimParser::Parse(amendment);
    CHECK(result.found_listing);
    CHECK_EQ(result.claims.size(), 5u);
    CHECK(result.confidence > 0.75);

    auto& c1 = result.claims[0];
    CHECK_EQ(c1.claim_number, 1);
    CHECK_EQ(static_cast<int>(c1.status), static_cast<int>(patx::ClaimStatus::CurrentlyAmended));
    CHECK(c1.is_independent);
    CHECK(c1.claim_text_clean.find("Currently Amended") == std::string::npos);
    CHECK(c1.claim_text_clean.find("A lithium battery cell") == 0);

    auto& c2 = result.claims[1];
    CHECK_EQ(static_cast<int>(c2.status), static_cast<int>(patx::ClaimStatus::New));
    CHECK(!c2.is_independent);
    CHECK_EQ(c2.parent_claim_numbers.size(), 1u);
    CHECK_EQ(c2.parent_claim_numbers[0], 1);

    auto& c3 = result.claims[2];
    CHECK_EQ(static_cast<int>(c3.status), static_cast<int>(patx::ClaimStatus::Canceled));
    CHECK(c3.claim_text_clean.empty());

    auto& c4 = result.claims[3];
    CHECK_EQ(static_cast<int>(c4.status), static_cast<int>(patx::ClaimStatus::PreviouslyPresented));
    CHECK_EQ(c4.parent_claim_numbers.size(), 2u);   // any one of claims 1-2

    // Remarks section after the claims must not bleed into claim 5
    auto& c5 = result.claims[4];
    CHECK(c5.claim_text_clean.find("REMARKS") == std::string::npos);
    CHECK(c5.claim_text_clean.find("traverses") == std::string::npos);
}

TEST(uspto_claim_parser_specification_claims) {
    std::string spec =
        "What is claimed is:\n1. A sensor array. 2. The sensor array of claim 1 "
        "wherein pixels are CMOS. 3. The sensor array of claims 1-2 further "
        "comprising a lens.";
    auto result = patx::ClaimParser::Parse(spec);
    CHECK(result.found_listing);
    CHECK_EQ(result.claims.size(), 3u);
    // No status identifiers in a spec -> originals, lower confidence
    CHECK_EQ(static_cast<int>(result.claims[0].status),
             static_cast<int>(patx::ClaimStatus::Original));
    CHECK(result.confidence < 0.5);
    CHECK_EQ(result.claims[2].parent_claim_numbers.size(), 2u);
}

TEST(uspto_claim_parser_no_listing) {
    auto result = patx::ClaimParser::Parse("Response arguments only, no claims.");
    CHECK(!result.found_listing);
    CHECK(!result.error.empty());
}

// ---------------- Claim diff ----------------

TEST(uspto_claim_diff_word_level) {
    using namespace patx;
    std::vector<Claim> v1, v2;
    Claim a1; a1.claim_number = 1; a1.claim_text_clean = "A device comprising a sensor and a processor."; a1.status = ClaimStatus::Original;
    Claim a2; a2.claim_number = 1; a2.claim_text_clean = "A device comprising a sensor, a processor and a filter."; a2.status = ClaimStatus::CurrentlyAmended;
    Claim a3; a3.claim_number = 2; a3.claim_text_clean = "The device of claim 1."; a3.status = ClaimStatus::New; a3.parent_claim_numbers = {1};
    v1 = {a1};
    v2 = {a2, a3};

    auto diffs = ClaimDiffEngine::DiffClaimSets(v1, v2);
    CHECK_EQ(diffs.size(), 2u);

    auto& d1 = diffs[0];
    CHECK_EQ(d1.claim_number, 1);
    CHECK(d1.both_present);
    CHECK(d1.added_words > 0);
    CHECK(d1.removed_words > 0);
    // Word-level, not char-level: segments must contain whole words
    bool has_added_and_unremoved = false;
    for (const auto& seg : d1.segments) {
        if (seg.op == DiffOp::Added && seg.text.find("filter") != std::string::npos)
            has_added_and_unremoved = true;
    }
    CHECK(has_added_and_unremoved);

    auto& d2 = diffs[1];
    CHECK(d2.added_claim);
    CHECK(d2.added_words > 0);
}

// ---------------- OA parser ----------------

TEST(uspto_oa_parser_rejections) {
    std::string oa_text =
        "MAIL DATE 2026-01-15\nEXAMINER, JANE SMITH\nART UNIT: 1727\n"
        "APPLICATION NO. 17248024\n"
        "NON-FINAL REJECTION\n"
        "Claims 1, 3 and 5-8 are rejected under 35 U.S.C. 103 as being unpatentable "
        "over U.S. Pat. No. 5,123,456 to Jones.\n"
        "Claim 2 is rejected under 35 U.S.C. 102 as anticipated by U.S. Pat. No. 6,654,321.\n"
        "Claims 9-10 are rejected under 35 U.S.C. 112(b) as being indefinite.\n"
        "Claim 4 is allowed.\n"
        "Applicant's amendment is non-compliant; see 112(a) written description rejection for claim 11.";

    auto result = patx::OaParser::Parse(oa_text);
    CHECK(result.is_office_action);
    CHECK_STR_EQ(result.oa_type, "Non-Final");
    CHECK_STR_EQ(result.mail_date, "2026-01-15");

    CHECK_EQ(result.claims_allowed.size(), 1u);
    CHECK_EQ(result.claims_allowed[0], 4);

    // Claim list parsing
    auto rejected = patx::OaParser::ParseClaimList("1, 3, 5-8");
    std::vector<int> expect{1, 3, 5, 6, 7, 8};
    CHECK(rejected == expect);

    // At least the big three statutes detected
    bool has_103 = false, has_102 = false, has_112b = false;
    for (const auto& r : result.rejections) {
        if (r.statute.find("103") != std::string::npos) has_103 = true;
        if (r.statute.find("102") != std::string::npos) has_102 = true;
        if (r.statute.find("112(b)") != std::string::npos) has_112b = true;
    }
    CHECK(has_103);
    CHECK(has_102);
    CHECK(has_112b);

    for (const auto& r : result.rejections) {
        if (r.statute.find("103") != std::string::npos) {
            CHECK_STR_EQ(r.primary_reference, "US 5,123,456 to Jones");
        }
    }
}

TEST(uspto_oa_parser_non_oa_text) {
    auto result = patx::OaParser::Parse("This is an amendment by applicant.");
    CHECK(!result.is_office_action);
}

// ---------------- Repository incremental dedup ----------------

TEST(uspto_repository_incremental_dedup) {
    std::string db_path = TempDbPath("uspto_repo");
    std::filesystem::remove(db_path);
    {
        Database db(db_path);
        patx::UsptoRepository repo(db.GetHandle());
        repo.EnsureTables();

        patx::UsptoCase c;
        c.application_number = "17248024";
        c.title = "Battery";
        int case_id = repo.UpsertCase(c);
        CHECK(case_id > 0);
        // Upserting again resolves to the same row
        patx::UsptoCase c2;
        c2.application_number = "17248024";
        c2.title = "Battery Cells";
        CHECK_EQ(repo.UpsertCase(c2), case_id);
        CHECK_STR_EQ(repo.GetCaseById(case_id).title, "Battery Cells");

        patx::UsptoDocument d;
        d.uspto_case_id = case_id;
        d.document_identifier = "LN4VBTHCXBLUEX2";
        d.document_code = "CTNF";
        d.document_category = "Non-Final Office Action";
        d.filing_date = "2026-01-15";
        d.source_last_modified_at = "2026-01-15T09:00:00Z";
        bool created = false;
        int doc_id = repo.UpsertDocument(d, &created);
        CHECK(doc_id > 0);
        CHECK(created);

        // Same identifier + same source timestamp -> no duplicate row
        patx::UsptoDocument d2 = d;
        bool created2 = true;
        CHECK_EQ(repo.UpsertDocument(d2, &created2), doc_id);
        CHECK(!created2);
        CHECK_EQ(repo.GetDocumentsForCase(case_id).size(), 1u);

        // Different identifier -> second row
        patx::UsptoDocument d3;
        d3.uspto_case_id = case_id;
        d3.document_identifier = "OTHERID123";
        d3.document_code = "IDS";
        d3.filing_date = "2026-02-01";
        repo.UpsertDocument(d3, &created2);
        CHECK(created2);
        CHECK_EQ(repo.GetDocumentsForCase(case_id).size(), 2u);
        CHECK_EQ(repo.CountNewDocuments(case_id), 2);
        CHECK(repo.MarkDocumentsSeen(case_id));
        CHECK_EQ(repo.CountNewDocuments(case_id), 0);

        // Claim version storage roundtrip
        patx::ClaimVersion v;
        v.uspto_case_id = case_id;
        v.version_no = 1;
        v.version_type = "initial";
        v.is_initial = true;
        int v_id = repo.InsertClaimVersion(v);
        CHECK(v_id > 0);
        patx::Claim claim;
        claim.claim_version_id = v_id;
        claim.claim_number = 1;
        claim.status = patx::ClaimStatus::CurrentlyAmended;
        claim.claim_text_clean = "A cell comprising an electrode.";
        claim.parent_claim_numbers = {};
        claim.is_independent = true;
        CHECK(repo.InsertClaim(claim) > 0);
        CHECK(repo.SetCurrentClaimVersion(case_id, v_id));

        auto versions = repo.GetClaimVersions(case_id);
        CHECK_EQ(versions.size(), 1u);
        CHECK(versions[0].is_initial);
        CHECK(versions[0].is_current);
        auto claims = repo.GetClaims(v_id);
        CHECK_EQ(claims.size(), 1u);
        CHECK_EQ(claims[0].claim_number, 1);
        CHECK_STR_EQ(claims[0].claim_text_clean, "A cell comprising an electrode.");
    }
    std::filesystem::remove(db_path);
}
