#include "core/log_rotation.h"

#include <cwchar>

namespace dockforge::core {

std::wstring LogFileNameFor(int year, int month, int day, int sequence) {
    wchar_t name[64]{};
    if (sequence > 0) {
        _snwprintf(name, 63, L"dockforge-%04d%02d%02d-%d.log", year, month, day, sequence);
    } else {
        _snwprintf(name, 63, L"dockforge-%04d%02d%02d.log", year, month, day);
    }
    return name;
}

bool ParseLogFileName(const std::wstring& name, LogFileDate& out) {
    // dockforge-YYYYMMDD.log        (sequence 0)
    // dockforge-YYYYMMDD-N.log      (sequence N, N >= 1, no zero padding)
    static const wchar_t* kPrefix = L"dockforge-";
    static const size_t kPrefixLen = 10;

    if (name.size() <= kPrefixLen + 9) return false;  // prefix + 8 digits + ".log"
    if (name.compare(0, kPrefixLen, kPrefix) != 0) return false;
    if (name.compare(name.size() - 4, 4, L".log") != 0) return false;

    const size_t stem_end = name.size() - 4;  // position of '.'
    const wchar_t* digits = name.c_str() + kPrefixLen;
    const size_t digit_count = stem_end - kPrefixLen;  // "YYYYMMDD" or "YYYYMMDD-N"

    // The date part is always exactly 8 digits.
    for (size_t i = 0; i < 8; ++i) {
        if (digits[i] < L'0' || digits[i] > L'9') return false;
    }
    int year = (digits[0] - L'0') * 1000 + (digits[1] - L'0') * 100 +
               (digits[2] - L'0') * 10 + (digits[3] - L'0');
    int month = (digits[4] - L'0') * 10 + (digits[5] - L'0');
    int day = (digits[6] - L'0') * 10 + (digits[7] - L'0');
    int sequence = 0;

    if (digit_count == 8) {
        // primary file
    } else if (digit_count > 9 && digits[8] == L'-') {
        // sequence part: at least one digit, all digits
        const size_t seq_len = digit_count - 9;
        for (size_t i = 0; i < seq_len; ++i) {
            const wchar_t c = digits[9 + i];
            if (c < L'0' || c > L'9') return false;
            sequence = sequence * 10 + (c - L'0');
        }
        if (sequence == 0) return false;  // "-0.log" is not a produced shape
    } else {
        return false;
    }

    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;

    out = LogFileDate{year, month, day, sequence};
    return true;
}

bool ShouldRollBySize(uint64_t current_bytes, uint64_t incoming_bytes, uint64_t cap) {
    if (cap == 0) return false;  // no cap configured: never roll
    if (current_bytes == 0) return false;  // an empty file accepts one line, always
    return current_bytes + incoming_bytes > cap;
}

int64_t DayOrdinal(int year, int month, int day) {
    // Howard Hinnant's days_from_civil - days since 1970-01-01, unsigned math
    // on the month-shift then era division; exact for the years we care about.
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);            // [0, 399]
    const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2) / 5 +   // [0, 153]
                         static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;              // [0, 146096]
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::vector<std::wstring> ExpiredLogFiles(
    const std::vector<std::pair<std::wstring, LogFileDate>>& files, int today_year,
    int today_month, int today_day, int keep_days) {
    std::vector<std::wstring> expired;
    if (keep_days < 1) return expired;  // a nonsensical policy deletes nothing

    const int64_t today = DayOrdinal(today_year, today_month, today_day);
    const int64_t oldest_kept = today - (keep_days - 1);

    for (const auto& [name, date] : files) {
        const int64_t ordinal = DayOrdinal(date.year, date.month, date.day);
        if (ordinal < oldest_kept) expired.push_back(name);
    }
    return expired;
}

}  // namespace dockforge::core
