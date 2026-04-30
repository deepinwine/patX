/**
 * patX Patent Data Structures
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Patent data structure
struct Patent {
    int32_t id = 0;
    char geke_code[16];
    char application_no[32];
    char title[256];
    char applicant[128];
    char application_date[16];
    char patent_type[32];
    char legal_status[32];
    char inventor[256];
    char classification[64];
    char agent[64];
    char agency[128];
    char publish_no[32];
    char publish_date[16];
    char grant_date[16];
    char expire_date[16];
    char handler[32];
    char notes[512];
    int32_t patent_level = 0;
    int32_t is_valid = 1;
    char created_at[32];
    char updated_at[32];
};

struct OaRecord {
    int32_t id = 0;
    char geke_code[16];
    char patent_title[256];
    char oa_type[32];
    char issue_date[16];
    char official_deadline[16];
    char handler[32];
    int32_t is_completed = 0;
    char created_at[32];
    char updated_at[32];
};

struct PctPatent {
    int32_t id = 0;
    char geke_code[16];
    char application_no[32];
    char title[256];
    char country[32];
    char application_status[32];
    char created_at[32];
    char updated_at[32];
};

struct Statistics {
    int32_t total_patents = 0;
    int32_t pending_oa = 0;
    int32_t urgent_oa = 0;
    int32_t by_type[10] = {0};
    int32_t by_status[10] = {0};
};