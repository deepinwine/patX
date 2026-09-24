// 期限计算底座 + CN 期限规则引擎（细则第4、5条）。
//
// 计算规则：
//   - 起算当天不计入期限（+N 天即第 N 天为截止日）
//   - 月/年期限取最后一月/年的“对应日”为截止日；没有对应日取该月最后一天
//   - 截止日落在法定休假日（含周末）顺延至之后第一个工作日
//
// 规则数据来源 data/deadline_rules/CN_deadline_rules.json（机器可读规则集）。
// 本模块不依赖 wx，可被核心库与离线测试直接链接。
#pragma once

#include <set>
#include <string>
#include <vector>

namespace patx {

class DeadlineEngine {
public:
    // 法定节假日表（"YYYY-MM-DD" 集合）；默认仅按周末顺延，
    // 国务院节假日安排可后续注入
    struct HolidayCalendar {
        std::set<std::string> extra_holidays;
        // 调休上班日（落在周末但按工作日处理）
        std::set<std::string> workday_overrides;
    };

    // ---- 纯日期运算（独立可测）----
    // 起算日不计入：base 的第 N 天 = base + N
    static std::string AddDays(const std::string& base, int days);
    // 对应日运算：同日下 N 月；无对应日收敛到月末（1-31 + 1月 = 2-28/29）
    static std::string AddMonths(const std::string& base, int months);
    static std::string AddYears(const std::string& base, int years);
    // 周末/节假日顺延
    static std::string AdjustForHolidays(const std::string& date,
                                         const HolidayCalendar* calendar = nullptr);
    static bool IsValidDate(const std::string& date);
    // 星期几：0=周日 .. 6=周六
    static int Weekday(const std::string& date);

    // ---- 规则模型（对应 CN_deadline_rules.json 单条规则）----
    struct Rule {
        std::string code;                 // CN_INV_FIRST_OA
        std::string procedure;            // SUBSTANTIVE_EXAMINATION
        std::string patent_type;          // INVENTION / UTILITY / DESIGN / ...
        std::string trigger_event;        // FIRST_OFFICE_ACTION
        std::string trigger_date_type;    // SERVICE_DATE / ISSUE_DATE / ...
        int period_value = 0;
        std::string period_unit;          // DAY / MONTH / YEAR
        std::string kind;                 // HARD_DEADLINE / OPEN_WINDOW / SLA
        std::string severity;             // 五级分类
        bool extendable = false;
        int max_extension_months = 0;
        int max_extension_count = 0;
        bool restorable = false;
        std::string restoration_type;
        std::string consequence;
        std::string effective_from;
        std::string legal_basis;
        std::string description;
        // 复合期限的替代分支（如“申请日起2个月 或 受理通知起15日”）
        struct Composite {
            std::string trigger_date_type;
            int period_value = 0;
            std::string period_unit;
        };
        bool has_composite = false;
        Composite composite;
        int composite_extra_days = 0;     // 附加天数（如“2个半月”的半月）
    };

    // 从 JSON 文件加载规则集；失败时返回 false 并给出 error
    static bool LoadRules(const std::string& json_path, std::vector<Rule>& rules,
                          std::string& error);

    // 按 code 精确查找（nullptr 当不存在）
    static const Rule* Find(const std::vector<Rule>& rules, const std::string& code);

    // 计算一条规则在给定起算日上的期限；起算日为空或规则无法计算返回 ""
    static std::string Compute(const Rule& rule, const std::string& trigger_date,
                               const HolidayCalendar* calendar = nullptr);
};

} // namespace patx
