// What Sellma downloads, from the runner's built-in WFC server
// ([network.wfc] provider = "built-in", ndsrecomp runner/src/title_dls.h):
// the downloadable quests and DQVC's stock, with no internet.
//
// Both are switches and numbers only: the quests, the special guests and
// the items are all on the cartridge.
//
//   quest52e.bin   the last quest file Nintendo served (4 June 2010): it
//                  switches on all the downloadable quests and special guests.
//   dqvc_dNNNNN.bin  today's DQVC stock: the sale running on the original
//                  calendar (dqvc_schedule.h), a share of its items picked
//                  for this day and this console, from which the game shows six.
//
// A download file is RC4-encrypted with the key "XENLONPROJECTKEY" and ends
// with 4 bytes that make the whole encrypted file, read as one big-endian
// number, a multiple of 0xC2A030D4. Decrypted, it is
//   u32 serial, u32 end of the records, u32 27, u32 2
//   records from offset 16: u8 tag, u8 0, u8 count, u8 the first four values'
//     2-bit types, then for values past four one 2-bit type each (16 per
//     word, unused bytes 0xFF), then the values (u32 each)
//   0xFF up to a multiple of 16, then "YYYY/MM/DD hh:mm:ss\0" "YYMMDD\0" and
//   0xFF up to 32 bytes.
#include <algorithm>
#include <array>
#include <iterator>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "dqvc_schedule.h"
#include "title_dls.h"

namespace {

// ---------------------------------------------------------------- file format
class DlsWriter {
public:
    explicit DlsWriter(uint32_t serial) : f_(16, 0) { Put32(0, serial); }

    void Record(uint8_t tag, const uint32_t* values, size_t n, uint8_t type) {
        f_.push_back(tag);
        f_.push_back(0);
        f_.push_back(static_cast<uint8_t>(n));
        uint8_t types = 0;
        for (size_t i = 0; i < n && i < 4; ++i) types |= static_cast<uint8_t>(type << (2 * i));
        f_.push_back(types);
        const size_t rest = n > 4 ? n - 4 : 0;
        const size_t at = f_.size();
        f_.resize(at + (rest + 15) / 16 * 4, 0xFF);
        for (size_t i = 0; i < rest; ++i) {
            uint8_t& b = f_[at + i / 4];
            if (i % 4 == 0) b = 0;
            b |= static_cast<uint8_t>(type << (2 * (i % 4)));
        }
        for (size_t i = 0; i < n; ++i) Append32(values[i]);
    }

    // `stamp` is "YYYY/MM/DD hh:mm:ss", `day` "YYMMDD".
    std::vector<uint8_t> Finish(const char* stamp, const char* day) {
        f_.resize((f_.size() + 15) / 16 * 16, 0xFF);
        const uint32_t end = static_cast<uint32_t>(f_.size());
        Put32(4, end);
        Put32(8, 27);
        Put32(12, 2);
        f_.resize(end + 32, 0xFF);
        std::memcpy(&f_[end], stamp, 19);
        f_[end + 19] = 0;
        std::memcpy(&f_[end + 20], day, 6);
        f_[end + 26] = 0;
        Seal();
        return std::move(f_);
    }

private:
    void Put32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) f_[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    void Append32(uint32_t v) {
        for (int i = 0; i < 4; ++i) f_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void Seal() {
        static const char kKey[] = "XENLONPROJECTKEY";
        uint8_t s[256];
        for (int i = 0; i < 256; ++i) s[i] = static_cast<uint8_t>(i);
        for (int i = 0, j = 0; i < 256; ++i) {
            j = (j + s[i] + static_cast<uint8_t>(kKey[i % 16])) & 255;
            std::swap(s[i], s[j]);
        }
        int i = 0, j = 0;
        for (uint8_t& b : f_) {
            i = (i + 1) & 255;
            j = (j + s[i]) & 255;
            std::swap(s[i], s[j]);
            b ^= s[(s[i] + s[j]) & 255];
        }
        // Four bytes that make the file, as one number, a multiple of M.
        constexpr uint64_t kM = 0xC2A030D4u;
        uint64_t r = 0;
        for (uint8_t b : f_) r = ((r << 8) + b) % kM;
        for (int k = 0; k < 4; ++k) r = (r << 8) % kM;
        const uint32_t x = static_cast<uint32_t>((kM - r) % kM);
        for (int k = 3; k >= 0; --k) f_.push_back(static_cast<uint8_t>(x >> (8 * k)));
    }

    std::vector<uint8_t> f_;
};

// ---------------------------------------------------------------- the quests
// The last quest file (USA, 4 June 2010): the downloadable quests it switched
// on, in the order they came out, and the special guests' order.
constexpr uint8_t kQuests[64] = {
    0x85, 0x86, 0xae, 0x91, 0xaf, 0x02, 0xb0, 0xb1, 0xb2, 0x95, 0xb3, 0x87, 0xb4,
    0x89, 0xb5, 0x8a, 0xb6, 0x9b, 0xb7, 0xa4, 0xb8, 0x7f, 0xb9, 0x9e, 0xba, 0xca,
    0xbc, 0xbd, 0xbe, 0xbf, 0x8f, 0x7b, 0xa1, 0x9c, 0xa5, 0xc0, 0xc1, 0x9a, 0xc2,
    0xc3, 0xc4, 0xc5, 0xc6, 0x94, 0x98, 0x96, 0x92, 0x9d, 0x99, 0xa2, 0x7a, 0xa0,
    0x83, 0x97, 0xa8, 0x8b, 0xa6, 0xad, 0x8c, 0xa9, 0xaa, 0x9f, 0x84, 0x88};
constexpr uint8_t kGuests[23] = {3,  19, 15, 4, 11, 1,  12, 21, 17, 8,  23, 16,
                                 6,  5,  7,  20, 13, 2, 9,  10, 14, 18, 22};

std::vector<uint8_t> QuestFile() {
    DlsWriter w(5);
    uint32_t v[64];
    v[0] = 0;
    w.Record(0x65, v, 1, 0);
    v[0] = 20;
    w.Record(0x64, v, 1, 0);
    for (int i = 0; i < 64; ++i) v[i] = kQuests[i];
    w.Record(0x6C, v, 64, 1);
    for (int i = 0; i < 23; ++i) v[i] = kGuests[i];
    w.Record(0x6D, v, 23, 1);
    v[0] = 1;
    w.Record(0x6E, v, 1, 1);
    return w.Finish("2010/06/04 17:42:30", "100604");
}

// ---------------------------------------------------------------- DQVC
// Days since 1970-01-01 (proleptic Gregorian).
int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// Each year's sales start on the Friday nearest 15 July.
int64_t CycleStart(int year) {
    const int64_t d = DaysFromCivil(year, 7, 15);
    int delta = 5 - static_cast<int>(((d + 4) % 7 + 7) % 7);  // 1970-01-01: Thursday
    if (delta > 3) delta -= 7;
    if (delta < -3) delta += 7;
    return d + delta;
}

constexpr size_t kDailyItems = 12;  // of the sale's items, per day and console

struct DqvcToday {
    std::string name;
    std::vector<uint8_t> data;
};

DqvcToday DqvcFile(const uint8_t mac[6]) {
    using namespace dq9::dqvc;
    const std::time_t now = std::time(nullptr);
    // The day changes at 08:00 UTC, when the original service put out each
    // day's stock.
    const int64_t day = FloorDiv(static_cast<int64_t>(now) - 8 * 3600, 86400);
    int year = 1970 + static_cast<int>(day / 366);
    while (DaysFromCivil(year + 1, 1, 1) <= day) ++year;
    int64_t cycle = CycleStart(year);
    if (day < cycle) cycle = CycleStart(--year);

    // The sale on now: the last one started, by its distance from its own
    // cycle's start (2011-07-15; the dates before 15 July are in 2012, the
    // last sale starts on 6 July).
    size_t sale = 0;
    for (size_t i = 0; i < std::size(kSales); ++i) {
        const int m = kSales[i].month, d = kSales[i].day;
        const int y = (m > 7 || (m == 7 && d >= 15)) ? 2011 : 2012;
        const int64_t start = DaysFromCivil(y, m, d) - DaysFromCivil(2011, 7, 15);
        if (start <= day - cycle) sale = i;
    }
    const Sale& s = kSales[sale];

    // This day's share for this console (the original's stock was different
    // for each player), the same all day, in the sale's order.
    std::vector<size_t> order(s.count);
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; ++i) h = (h ^ mac[i]) * 16777619u;
    for (int i = 0; i < 8; ++i) h = (h ^ static_cast<uint8_t>(day >> (8 * i))) * 16777619u;
    for (size_t i = order.size(); i > 1; --i) {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        std::swap(order[i - 1], order[h % i]);
    }
    std::vector<bool> keep(s.count, order.size() <= kDailyItems);
    for (size_t i = 0; i < kDailyItems && i < order.size(); ++i) keep[order[i]] = true;

    // A new serial every day, so the game takes it as new stock (it refuses
    // serials past 16 bits).
    const int64_t abs_day = day - DaysFromCivil(2010, 7, 1);
    DlsWriter w(static_cast<uint32_t>(1000 + abs_day));
    uint32_t v[4] = {0};
    w.Record(0x65, v, 1, 0);
    v[0] = 20;
    w.Record(0x64, v, 1, 0);
    v[0] = 2099;  // on sale until
    v[1] = 12;
    v[2] = 31;
    w.Record(0x67, v, 3, 1);
    size_t shown = 0;
    for (size_t i = 0; i < s.count; ++i) {
        if (!keep[i]) continue;
        const Item& it = kItems[s.first + i];
        v[0] = it.id;
        v[1] = it.price;
        v[2] = 1;
        v[3] = it.qty ? it.qty : 1;
        w.Record(0x68, v, 4, 1);
        ++shown;
    }
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char stamp[32], ymd[16];
    std::strftime(stamp, sizeof stamp, "%Y/%m/%d %H:%M:%S", &tm);
    std::strftime(ymd, sizeof ymd, "%y%m%d", &tm);

    DqvcToday out;
    char name[32];
    std::snprintf(name, sizeof name, "dqvc_d%05lld.bin", static_cast<long long>(abs_day));
    out.name = name;
    out.data = w.Finish(stamp, ymd);
    std::fprintf(stderr, "[wfc] DQVC: sale %zu of 52, %zu of its %u items today\n",
                 sale + 1, shown, static_cast<unsigned>(s.count));
    return out;
}

// ---------------------------------------------------------------- the server's query
void Downloads(const NdsDlsRequest& request, std::vector<NdsDlsFile>* files) {
    // USA and Europe: the same quests and items.
    if (request.gamecd != "YDQE" && request.gamecd != "YDQP") return;
    NdsDlsFile quests;
    quests.name = "quest52e.bin";
    quests.attr1 = "quest";
    quests.data = QuestFile();
    files->push_back(std::move(quests));

    DqvcToday today = DqvcFile(request.mac);
    NdsDlsFile stock;
    stock.name = std::move(today.name);
    stock.attr1 = "auction";
    stock.data = std::move(today.data);
    files->push_back(std::move(stock));
}

const bool kRegistered = (nds_title_set_dls_query(&Downloads), true);

}  // namespace
