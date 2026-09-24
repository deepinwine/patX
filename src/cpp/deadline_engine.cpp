// 期限计算底座实现。日期一律为 "YYYY-MM-DD"；不依赖平台时间函数，
// 星期用 Zeller 同余计算，保证跨平台结果一致。
#include "patx/deadline_engine.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace patx {

namespace {

int DaysInMonth(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return days[m - 1];
}

bool ParseDate(const std::string& s, int& y, int& m, int& d) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    auto digits = [](const std::string& t, size_t from, size_t n) -> int {
        int v = 0;
        for (size_t i = 0; i < n; ++i) {
            char c = t[from + i];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    y = digits(s, 0, 4);
    m = digits(s, 5, 2);
    d = digits(s, 8, 2);
    if (y < 1900 || m < 1 || m > 12 || d < 1) return false;
    return d <= DaysInMonth(y, m);
}

std::string FormatDate(int y, int m, int d) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

// 序号日期互转（civil days 算法，Howard Hinnant days_from_civil）
long long ToSerial(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 + static_cast<int>(doe) - 719468;
}

void FromSerial(long long serial, int& y, int& m, int& d) {
    serial += 719468;
    const int era = static_cast<int>((serial >= 0 ? serial : serial - 146096) / 146097);
    const unsigned doe = static_cast<unsigned>(serial - static_cast<long long>(era) * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    if (m <= 2) ++y;
}

} // namespace

bool DeadlineEngine::IsValidDate(const std::string& date) {
    int y, m, d;
    return ParseDate(date, y, m, d);
}

int DeadlineEngine::Weekday(const std::string& date) {
    int y, m, d;
    if (!ParseDate(date, y, m, d)) return -1;
    // 1970-01-01 是周四；serial 0 = 1970-01-01（周四）
    long long serial = ToSerial(y, m, d);
    int wd = (static_cast<int>((serial % 7 + 7) % 7) + 4) % 7;   // 0=周日
    return wd;
}

std::string DeadlineEngine::AddDays(const std::string& base, int days) {
    int y, m, d;
    if (!ParseDate(base, y, m, d) || days < 0) return "";
    long long serial = ToSerial(y, m, d) + days;
    FromSerial(serial, y, m, d);
    return FormatDate(y, m, d);
}

std::string DeadlineEngine::AddMonths(const std::string& base, int months) {
    int y, m, d;
    if (!ParseDate(base, y, m, d) || months < 0) return "";
    long long total = static_cast<long long>(y) * 12 + (m - 1) + months;
    int y2 = static_cast<int>(total / 12);
    int m2 = static_cast<int>(total % 12) + 1;
    // 对应日：没有对应日取该月最后一日
    int d2 = std::min(d, DaysInMonth(y2, m2));
    return FormatDate(y2, m2, d2);
}

std::string DeadlineEngine::AddYears(const std::string& base, int years) {
    int y, m, d;
    if (!ParseDate(base, y, m, d) || years < 0) return "";
    int y2 = y + years;
    // 2-29 的年对应日收敛到 2-28/29
    int d2 = std::min(d, DaysInMonth(y2, m));
    return FormatDate(y2, m, d2);
}

std::string DeadlineEngine::AdjustForHolidays(const std::string& date,
                                              const HolidayCalendar* calendar) {
    if (!IsValidDate(date)) return "";
    std::string adjusted = date;
    for (int guard = 0; guard < 30; ++guard) {   // 连续长假保护上限
        int wd = Weekday(adjusted);
        bool weekend = wd == 0 || wd == 6;
        bool extra = calendar &&
                     calendar->extra_holidays.count(adjusted) > 0;
        bool workday_override = calendar &&
                                calendar->workday_overrides.count(adjusted) > 0;
        if ((!workday_override) && (weekend || extra)) {
            adjusted = AddDays(adjusted, 1);
        } else {
            break;
        }
    }
    return adjusted;
}

bool DeadlineEngine::LoadRules(const std::string& json_path, std::vector<Rule>& rules,
                               std::string& error) {
    rules.clear();
    std::ifstream in(json_path);
    if (!in.is_open()) {
        error = "无法打开规则文件: " + json_path;
        return false;
    }
    try {
        auto root = nlohmann::json::parse(in);
        if (!root.contains("rules") || !root["rules"].is_array()) {
            error = "规则文件缺少 rules 数组";
            return false;
        }
        for (const auto& r : root["rules"]) {
            Rule rule;
            rule.code = r.value("code", "");
            rule.procedure = r.value("procedure", "");
            rule.patent_type = r.value("patent_type", "");
            rule.trigger_event = r.value("trigger_event", "");
            rule.trigger_date_type = r.value("trigger_date_type", "");
            rule.period_value = r.value("period_value", 0);
            rule.period_unit = r.value("period_unit", "");
            rule.kind = r.value("kind", "");
            rule.severity = r.value("severity", "");
            rule.extendable = r.value("extendable", false);
            rule.max_extension_months = r.value("max_extension_months", 0);
            rule.max_extension_count = r.value("max_extension_count", 0);
            rule.restorable = r.value("restorable", false);
            rule.restoration_type = r.value("restoration_type", "");
            rule.consequence = r.value("consequence", "");
            rule.effective_from = r.value("effective_from", "");
            rule.legal_basis = r.value("legal_basis", "");
            rule.description = r.value("description", "");
            if (r.contains("composite_alt") && r["composite_alt"].is_object()) {
                rule.has_composite = true;
                rule.composite.trigger_date_type =
                    r["composite_alt"].value("trigger_date_type", "");
                rule.composite.period_value = r["composite_alt"].value("period_value", 0);
                rule.composite.period_unit = r["composite_alt"].value("period_unit", "");
            }
            rule.composite_extra_days = r.value("composite_extra_days", 0);
            if (!rule.code.empty()) rules.push_back(std::move(rule));
        }
        return true;
    } catch (const std::exception& exc) {
        error = std::string("规则文件解析失败: ") + exc.what();
        return false;
    }
}

const DeadlineEngine::Rule* DeadlineEngine::Find(const std::vector<Rule>& rules,
                                                 const std::string& code) {
    for (const auto& r : rules) {
        if (r.code == code) return &r;
    }
    return nullptr;
}

std::string DeadlineEngine::Compute(const Rule& rule, const std::string& trigger_date,
                                    const HolidayCalendar* calendar) {
    if (!IsValidDate(trigger_date)) return "";
    std::string result;
    if (rule.period_unit == "DAY") {
        result = AddDays(trigger_date, rule.period_value);
    } else if (rule.period_unit == "MONTH") {
        result = AddMonths(trigger_date, rule.period_value);
    } else if (rule.period_unit == "YEAR") {
        result = AddYears(trigger_date, rule.period_value);
    } else {
        return "";
    }
    if (rule.composite_extra_days > 0 && !result.empty()) {
        result = AddDays(result, rule.composite_extra_days);
    }
    // SLA / OPEN_WINDOW 不做节假日顺延（非法定硬期限）；HARD_DEADLINE 顺延
    if (!result.empty() && rule.kind == "HARD_DEADLINE") {
        result = AdjustForHolidays(result, calendar);
    }
    return result;
}

} // namespace patx
