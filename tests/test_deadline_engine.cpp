// 期限计算底座与 CN 规则引擎测试。纯离线：日历运算 + 规则 JSON 加载。
#include "patx/deadline_engine.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using patx::DeadlineEngine;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "      \
                      << #cond << std::endl;                                 \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                   \
    do {                                                                     \
        if ((a) != (b)) {                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  '"     \
                      << (a) << "' != '" << (b) << "'" << std::endl;         \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void TestCalendarBase() {
    // 起算日不计入：第 15 天 = base + 15
    CHECK_STR_EQ(DeadlineEngine::AddDays("2026-09-01", 15), "2026-09-16");

    // 对应日
    CHECK_STR_EQ(DeadlineEngine::AddMonths("2026-05-23", 4), "2026-09-23");
    // 月末收敛：1-31 + 1月 = 2-28（非闰月）；SQLite date() 会错算成 3-03
    CHECK_STR_EQ(DeadlineEngine::AddMonths("2026-01-31", 1), "2026-02-28");
    CHECK_STR_EQ(DeadlineEngine::AddMonths("2024-01-31", 1), "2024-02-29");  // 闰年
    // 8-31 + 6月 = 2-28/29
    CHECK_STR_EQ(DeadlineEngine::AddMonths("2025-08-31", 6), "2026-02-28");
    // 跨年
    CHECK_STR_EQ(DeadlineEngine::AddMonths("2026-11-20", 3), "2027-02-20");
    // 年：2-29 闰日收敛
    CHECK_STR_EQ(DeadlineEngine::AddYears("2024-02-29", 1), "2025-02-28");
    CHECK_STR_EQ(DeadlineEngine::AddYears("2024-06-30", 20), "2044-06-30");

    // 星期：1970-01-01 周四；2026-09-24 周四
    CHECK(DeadlineEngine::Weekday("1970-01-01") == 4);
    CHECK(DeadlineEngine::Weekday("2026-09-24") == 4);
    CHECK(DeadlineEngine::Weekday("2026-09-26") == 6);   // 周六
    CHECK(DeadlineEngine::Weekday("2026-09-27") == 0);   // 周日

    // 节假日顺延：周六 -> 周一
    CHECK_STR_EQ(DeadlineEngine::AdjustForHolidays("2026-11-14"), "2026-11-16");
    // 周日 -> 周一
    CHECK_STR_EQ(DeadlineEngine::AdjustForHolidays("2026-11-15"), "2026-11-16");
    // 工作日不动
    CHECK_STR_EQ(DeadlineEngine::AdjustForHolidays("2026-11-18"), "2026-11-18");

    // 法定节假日注入：2026-10-01（周四）设为假日 -> 顺延到 10-02（周五）
    DeadlineEngine::HolidayCalendar cal;
    cal.extra_holidays.insert("2026-10-01");
    CHECK_STR_EQ(DeadlineEngine::AdjustForHolidays("2026-10-01", &cal), "2026-10-02");
    // 调休上班日：周六但按工作日处理则不顺延
    DeadlineEngine::HolidayCalendar cal2;
    cal2.workday_overrides.insert("2026-11-14");
    CHECK_STR_EQ(DeadlineEngine::AdjustForHolidays("2026-11-14", &cal2), "2026-11-14");

    // 非法输入
    CHECK(!DeadlineEngine::IsValidDate("2026-02-30"));
    CHECK(!DeadlineEngine::IsValidDate("2026-13-01"));
    CHECK(DeadlineEngine::AddDays("bad", 1).empty());
}

static void TestRulesJson() {
    // 规则文件在仓库 data/deadline_rules/ 下；测试从可执行文件位置向上找
    namespace fs = std::filesystem;
    fs::path rules_path = fs::path(__FILE__).parent_path().parent_path() /
                          "data" / "deadline_rules" / "CN_deadline_rules.json";
    std::vector<DeadlineEngine::Rule> rules;
    std::string error;
    if (!DeadlineEngine::LoadRules(rules_path.string(), rules, error)) {
        std::cerr << "load rules failed: " << error << std::endl;
        CHECK(false);
        return;
    }
    CHECK(rules.size() >= 60);   // 完整规则集

    // 一通 4 个月：2026-05-23 -> 2026-09-23（周三，不顺延）
    const auto* first_oa = DeadlineEngine::Find(rules, "CN_INV_FIRST_OA");
    CHECK(first_oa != nullptr);
    CHECK_STR_EQ(DeadlineEngine::Compute(*first_oa, "2026-05-23"), "2026-09-23");

    // 后续 OA 2 个月 + 周日顺延：2026-09-18 -> 2026-11-18（周三）
    const auto* further = DeadlineEngine::Find(rules, "CN_INV_FURTHER_OA");
    CHECK(further != nullptr);
    CHECK_STR_EQ(DeadlineEngine::Compute(*further, "2026-09-18"), "2026-11-18");
    // 2026-07-15 + 2月 = 2026-09-15（周二）
    CHECK_STR_EQ(DeadlineEngine::Compute(*further, "2026-07-15"), "2026-09-15");

    // 优先审查（2026-09-01 新规）：发文日起 1 个月
    const auto* prio = DeadlineEngine::Find(rules, "CN_PRIORITY_EXAM_INV_OA");
    CHECK(prio != nullptr);
    CHECK_STR_EQ(prio->trigger_date_type, "ISSUE_DATE");
    CHECK_STR_EQ(DeadlineEngine::Compute(*prio, "2026-09-24"), "2026-10-26");   // 10-24 为周六，顺延
    CHECK(!prio->extendable);

    // 优先审查实用新型：15 日
    const auto* prio_um = DeadlineEngine::Find(rules, "CN_PRIORITY_EXAM_UM_OA");
    CHECK(prio_um != nullptr);
    CHECK_STR_EQ(DeadlineEngine::Compute(*prio_um, "2026-09-24"), "2026-10-09");

    // 优先权 12 个月（对应日）
    const auto* prio12 = DeadlineEngine::Find(rules, "CN_PRIORITY_INV_UM");
    CHECK_STR_EQ(DeadlineEngine::Compute(*prio12, "2025-09-24"), "2026-09-24");
    // 月末收敛：2025-08-31 + 12 = 2026-08-31
    CHECK_STR_EQ(DeadlineEngine::Compute(*prio12, "2025-08-31"), "2026-08-31");

    // PCT 30 个月
    const auto* pct = DeadlineEngine::Find(rules, "CN_PCT_ENTRY_NORMAL");
    CHECK_STR_EQ(DeadlineEngine::Compute(*pct, "2024-05-15"), "2026-11-16");   // 11-15 为周日，顺延

    // 复审 3 个月
    const auto* reexam = DeadlineEngine::Find(rules, "CN_REEXAM_REQUEST");
    CHECK_STR_EQ(DeadlineEngine::Compute(*reexam, "2026-09-24"), "2026-12-24");

    // 年费期限：发明 20 年
    const auto* term = DeadlineEngine::Find(rules, "CN_ANNUITY_TERM_INV");
    CHECK_STR_EQ(DeadlineEngine::Compute(*term, "2026-05-23"), "2046-05-23");

    // 顺延落在 HARD_DEADLINE 上生效：构造 4 个月后为周日的情形
    // 2026-07-15 + 4月 = 2026-11-15（周日）-> 2026-11-16
    CHECK_STR_EQ(DeadlineEngine::Compute(*first_oa, "2026-07-15"), "2026-11-16");

    // SLA 不顺延：公开 18 个月节点（kind=SLA）
    const auto* pub = DeadlineEngine::Find(rules, "CN_PUBLICATION_18M");
    CHECK_STR_EQ(DeadlineEngine::Compute(*pub, "2025-05-31"), "2026-11-30");
}

int main() {
    TestCalendarBase();
    TestRulesJson();
    if (g_failures == 0) {
        std::cout << "all deadline engine tests passed" << std::endl;
        return 0;
    }
    std::cout << g_failures << " failure(s)" << std::endl;
    return 1;
}
