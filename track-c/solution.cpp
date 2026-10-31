#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <chrono>

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

// Format cents: -525 -> "-5.25", -5 -> "-0.05", 0 -> "0.00"
inline std::string format_cents(int64_t cents) {
    std::string s;
    if (cents < 0) {
        s += '-';
        cents = -cents;
    }
    s += std::to_string(cents / 100);
    s += '.';
    int64_t rem = cents % 100;
    if (rem < 10) s += '0';
    s += std::to_string(rem);
    return s;
}

// Fast string hashing (Murmur/FNV style)
inline uint64_t hash_str(const char* data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Global String Interner
// Memory arena for deduplicated strings
struct StringInterner {
    struct Slot {
        uint32_t offset;
        uint16_t len;
        bool occupied;
        uint8_t pad;
        uint64_t hash;
    };

    std::vector<char> pool;
    std::vector<Slot> slots; // hash table with open addressing
    uint32_t mask;
    uint32_t count = 0;

    StringInterner(size_t cap = 262144) {
        pool.reserve(4 * 1024 * 1024); // 4 MB pool
        // Cap is power of 2
        slots.resize(cap, {0, 0, false, 0, 0});
        mask = static_cast<uint32_t>(cap - 1);
    }

    void grow() {
        size_t new_cap = slots.size() * 2;
        std::vector<Slot> new_slots(new_cap, {0, 0, false, 0, 0});
        uint32_t new_mask = static_cast<uint32_t>(new_cap - 1);
        for (const auto& slot : slots) {
            if (slot.occupied) {
                uint32_t idx = static_cast<uint32_t>(slot.hash) & new_mask;
                while (new_slots[idx].occupied) {
                    idx = (idx + 1) & new_mask;
                }
                new_slots[idx] = slot;
            }
        }
        slots = std::move(new_slots);
        mask = new_mask;
    }

    uint32_t intern(const char* s, size_t len, uint64_t h) {
        uint32_t idx = static_cast<uint32_t>(h) & mask;
        while (slots[idx].occupied) {
            if (slots[idx].len == len && slots[idx].hash == h &&
                (len == 0 || std::memcmp(pool.data() + slots[idx].offset, s, len) == 0)) {
                return idx; // Found existing
            }
            idx = (idx + 1) & mask;
        }

        // Insert new string into pool
        uint32_t offset = static_cast<uint32_t>(pool.size());
        if (len > 0) {
            pool.insert(pool.end(), s, s + len);
        }

        slots[idx] = {offset, static_cast<uint16_t>(len), true, 0, h};
        count++;

        if (count * 10 >= slots.size() * 7) {
            grow();
            // Look up the slot again in the resized table to return its new index
            idx = static_cast<uint32_t>(h) & mask;
            while (slots[idx].occupied) {
                if (slots[idx].len == len && slots[idx].hash == h &&
                    (len == 0 || std::memcmp(pool.data() + slots[idx].offset, s, len) == 0)) {
                    return idx;
                }
                idx = (idx + 1) & mask;
            }
        }

        return idx;
    }
};

// Compact flat hash set for interned string IDs per country
struct FlatIdSet {
    std::vector<uint32_t> table; // stores intern_id + 1 (0 is empty)
    uint32_t mask;
    uint32_t size_count = 0;

    FlatIdSet(size_t initial_cap = 64) {
        table.assign(initial_cap, 0);
        mask = static_cast<uint32_t>(initial_cap - 1);
    }

    void insert(uint32_t id) {
        uint32_t val = id + 1;
        uint32_t idx = (id * 2654435761U) & mask;
        while (table[idx] != 0) {
            if (table[idx] == val) return;
            idx = (idx + 1) & mask;
        }
        table[idx] = val;
        size_count++;
        if (size_count * 2 > table.size()) {
            grow();
        }
    }

    void grow() {
        size_t new_cap = table.size() * 2;
        std::vector<uint32_t> new_table(new_cap, 0);
        uint32_t new_mask = static_cast<uint32_t>(new_cap - 1);
        for (uint32_t val : table) {
            if (val != 0) {
                uint32_t id = val - 1;
                uint32_t idx = (id * 2654435761U) & new_mask;
                while (new_table[idx] != 0) {
                    idx = (idx + 1) & new_mask;
                }
                new_table[idx] = val;
            }
        }
        table = std::move(new_table);
        mask = new_mask;
    }

    size_t size() const {
        return size_count;
    }
};

// Country running aggregates
struct CountryAgg {
    std::string name;
    uint64_t count = 0;
    uint64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    FlatIdSet distinct_name_ids;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    bool active = false;

    inline void add(uint64_t record_id, uint32_t name_id, size_t unescaped_name_len, size_t unescaped_note_len, int64_t cents) {
        count++;
        id_sum += record_id;
        amount_sum += cents;
        if (cents < amount_min) amount_min = cents;
        if (cents > amount_max) amount_max = cents;
        name_bytes += unescaped_name_len;
        note_bytes += unescaped_note_len;
        distinct_name_ids.insert(name_id);
    }
};

// Fast integer parsing
inline uint64_t parse_uint(const char* p, const char* end) {
    uint64_t val = 0;
    for (; p < end; ++p) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
        }
    }
    return val;
}

// Fast decimal cents parsing: (-?)([0-9]+)\.([0-9]{2})
inline int64_t parse_cents(const char* p, const char* end) {
    bool neg = false;
    if (p < end && *p == '-') {
        neg = true;
        ++p;
    }
    int64_t whole = 0;
    for (; p < end && *p != '.'; ++p) {
        if (*p >= '0' && *p <= '9') {
            whole = whole * 10 + (*p - '0');
        }
    }
    int64_t frac = 0;
    if (p < end && *p == '.') {
        ++p;
        if (p < end) frac += (*p++ - '0') * 10;
        if (p < end) frac += (*p++ - '0');
    }
    int64_t total = whole * 100 + frac;
    return neg ? -total : total;
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    auto t_start = clock_type::now();

    std::string data_dir = "/data";
    std::string out_dir = "/out";

    if (argc >= 2) data_dir = argv[1];
    if (argc >= 3) out_dir = argv[2];

    fs::path regions_path = fs::path(data_dir) / "regions.csv";
    fs::path records_path = fs::path(data_dir) / "records.csv";
    fs::path output_path = fs::path(out_dir) / "aggregates.txt";

    // 1. Load regions.csv into flat direct-lookup table
    static constexpr size_t MAX_REGIONS = 1000000;
    static constexpr uint16_t UNMATCHED_ID = 0xFFFF;
    std::vector<uint16_t> region_to_country(MAX_REGIONS, UNMATCHED_ID);

    std::vector<CountryAgg> countries;
    countries.reserve(256);

    auto get_country_id = [&](std::string_view cname) -> uint16_t {
        for (uint16_t i = 0; i < countries.size(); ++i) {
            if (countries[i].name == cname) return i;
        }
        uint16_t new_id = static_cast<uint16_t>(countries.size());
        countries.emplace_back();
        countries.back().name = std::string(cname);
        return new_id;
    };

    uint16_t unmatched_country_id = get_country_id("_UNMATCHED");

    {
        std::ifstream rf(regions_path);
        if (!rf.is_open()) return 1;
        std::string line;
        // Skip header
        if (std::getline(rf, line)) {
            while (std::getline(rf, line)) {
                if (line.empty() || line[0] == '\r') continue;
                size_t comma = line.find(',');
                if (comma == std::string::npos) continue;
                uint64_t rid = parse_uint(line.data(), line.data() + comma);
                size_t end_c = line.find_first_of("\r\n", comma + 1);
                if (end_c == std::string::npos) end_c = line.size();
                std::string_view cname(line.data() + comma + 1, end_c - (comma + 1));
                uint16_t cid = get_country_id(cname);
                if (rid < MAX_REGIONS) {
                    region_to_country[rid] = cid;
                }
            }
        }
    }

    // 2. Mmap records.csv
    int fd = open(records_path.c_str(), O_RDONLY);
    if (fd < 0) return 1;

    struct stat sb;
    if (fstat(fd, &sb) < 0) { close(fd); return 1; }

    size_t file_size = sb.st_size;
    if (file_size == 0) {
        close(fd);
        fs::create_directories(output_path.parent_path());
        std::ofstream out(output_path, std::ios::binary);
        return 0;
    }

    const char* data = static_cast<const char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    madvise(const_cast<char*>(data), file_size, MADV_SEQUENTIAL);

    const char* ptr = data;
    const char* end = data + file_size;

    // Check BOM at start of file (\xEF\xBB\xBF)
    if (end - ptr >= 3 && static_cast<unsigned char>(ptr[0]) == 0xEF &&
        static_cast<unsigned char>(ptr[1]) == 0xBB && static_cast<unsigned char>(ptr[2]) == 0xBF) {
        ptr += 3;
    }

    // Skip CSV header line: id,name,note,region_id,amount\n
    while (ptr < end && *ptr != '\n') ptr++;
    if (ptr < end && *ptr == '\n') ptr++;

    StringInterner interner;
    std::string unescape_scratch;
    unescape_scratch.reserve(1024);

    // 3. RFC-Compliant Streaming CSV Parser
    while (ptr < end) {
        // Strip BOM if present right after a newline
        if (end - ptr >= 3 && static_cast<unsigned char>(ptr[0]) == 0xEF &&
            static_cast<unsigned char>(ptr[1]) == 0xBB && static_cast<unsigned char>(ptr[2]) == 0xBF) {
            ptr += 3;
        }
        if (ptr >= end || *ptr == '\r' || *ptr == '\n') {
            while (ptr < end && (*ptr == '\r' || *ptr == '\n')) ptr++;
            continue;
        }

        // Field 0: id (unquoted integer)
        const char* p_id_start = ptr;
        while (ptr < end && *ptr != ',') ptr++;
        uint64_t record_id = parse_uint(p_id_start, ptr);
        if (ptr < end && *ptr == ',') ptr++;

        // Field 1: name (may be quoted, can have escaped quotes "")
        const char* p_name_start = ptr;
        const char* p_name_end = ptr;
        uint32_t name_escaped_quotes = 0;
        if (ptr < end && *ptr == '"') {
            ptr++;
            p_name_start = ptr;
            while (ptr < end) {
                if (*ptr == '"') {
                    if (ptr + 1 < end && *(ptr + 1) == '"') {
                        name_escaped_quotes++;
                        ptr += 2;
                    } else {
                        p_name_end = ptr;
                        ptr++; // consume closing quote
                        break;
                    }
                } else {
                    ptr++;
                }
            }
            if (ptr < end && *ptr == ',') ptr++;
        } else {
            while (ptr < end && *ptr != ',') ptr++;
            p_name_end = ptr;
            if (ptr < end && *ptr == ',') ptr++;
        }
        size_t raw_name_len = p_name_end - p_name_start;
        size_t unescaped_name_len = raw_name_len - name_escaped_quotes;

        const char* unescaped_name_ptr = p_name_start;
        if (name_escaped_quotes > 0) {
            unescape_scratch.clear();
            unescape_scratch.reserve(unescaped_name_len);
            for (size_t i = 0; i < raw_name_len; ++i) {
                if (p_name_start[i] == '"' && i + 1 < raw_name_len && p_name_start[i + 1] == '"') {
                    unescape_scratch += '"';
                    ++i;
                } else {
                    unescape_scratch += p_name_start[i];
                }
            }
            unescaped_name_ptr = unescape_scratch.data();
        }

        uint64_t nh = hash_str(unescaped_name_ptr, unescaped_name_len);
        uint32_t name_id = interner.intern(unescaped_name_ptr, unescaped_name_len, nh);

        // Field 2: note (may be quoted, can have escaped quotes and embedded newlines)
        const char* p_note_start = ptr;
        const char* p_note_end = ptr;
        uint32_t note_escaped_quotes = 0;
        if (ptr < end && *ptr == '"') {
            ptr++;
            p_note_start = ptr;
            while (ptr < end) {
                if (*ptr == '"') {
                    if (ptr + 1 < end && *(ptr + 1) == '"') {
                        note_escaped_quotes++;
                        ptr += 2;
                    } else {
                        p_note_end = ptr;
                        ptr++; // consume closing quote
                        break;
                    }
                } else {
                    ptr++;
                }
            }
            if (ptr < end && *ptr == ',') ptr++;
        } else {
            while (ptr < end && *ptr != ',') ptr++;
            p_note_end = ptr;
            if (ptr < end && *ptr == ',') ptr++;
        }
        size_t raw_note_len = p_note_end - p_note_start;
        size_t unescaped_note_len = raw_note_len - note_escaped_quotes;

        // Field 3: region_id (unquoted integer)
        const char* p_reg_start = ptr;
        while (ptr < end && *ptr != ',') ptr++;
        uint64_t region_id = parse_uint(p_reg_start, ptr);
        if (ptr < end && *ptr == ',') ptr++;

        // Field 4: amount (signed decimal)
        const char* p_amt_start = ptr;
        while (ptr < end && *ptr != '\n' && *ptr != '\r') ptr++;
        int64_t cents = parse_cents(p_amt_start, ptr);
        while (ptr < end && (*ptr == '\r' || *ptr == '\n')) ptr++;

        // Join
        uint16_t cid = (region_id < MAX_REGIONS) ? region_to_country[region_id] : UNMATCHED_ID;
        if (cid == UNMATCHED_ID) cid = unmatched_country_id;

        CountryAgg& c = countries[cid];
        c.active = true;
        c.add(record_id, name_id, unescaped_name_len, unescaped_note_len, cents);
    }

    munmap(const_cast<char*>(data), file_size);
    close(fd);

    // 4. Sort countries lexicographically
    std::vector<CountryAgg*> active_countries;
    for (CountryAgg& c : countries) {
        if (c.active) {
            active_countries.push_back(&c);
        }
    }

    std::sort(active_countries.begin(), active_countries.end(), [](const CountryAgg* a, const CountryAgg* b) {
        return a->name < b->name;
    });

    // 5. Output serialization
    fs::create_directories(output_path.parent_path());
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) return 1;

    std::string out_buf;
    out_buf.reserve(64 * 1024);

    for (const CountryAgg* c : active_countries) {
        out_buf += c->name;
        out_buf += ' ';
        out_buf += std::to_string(c->count);
        out_buf += ' ';
        out_buf += std::to_string(c->id_sum);
        out_buf += ' ';
        out_buf += format_cents(c->amount_sum);
        out_buf += ' ';
        out_buf += format_cents(c->amount_min);
        out_buf += ' ';
        out_buf += format_cents(c->amount_max);
        out_buf += ' ';
        out_buf += std::to_string(c->distinct_name_ids.size());
        out_buf += ' ';
        out_buf += std::to_string(c->name_bytes);
        out_buf += ' ';
        out_buf += std::to_string(c->note_bytes);
        out_buf += '\n';
    }

    out.write(out_buf.data(), out_buf.size());
    out.flush();

    auto t_end = clock_type::now();
    double exec_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);

    std::cerr << "TRACK_C_EXPERIMENT_RESULT: exec_ms=" << exec_ms
              << " peak_rss_kb=" << usage.ru_maxrss
              << " active_countries=" << active_countries.size()
              << " distinct_names=" << interner.count
              << "\n";

    return 0;
}
