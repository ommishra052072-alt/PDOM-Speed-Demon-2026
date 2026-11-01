// candidate_c03.cpp - Track C High-Throughput Parallel Ingest with Compact Memory Architecture
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
#include <thread>
#include <chrono>
#include <atomic>

namespace fs = std::filesystem;
using clock_type = std::chrono::high_resolution_clock;

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

inline uint64_t hash_bytes(const char* data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL ^ (len * 0x517cc1b727220a95ULL);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        h ^= v;
        h *= 0x100000001b3ULL;
        p += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        h ^= v;
        h *= 0x100000001b3ULL;
        p += 4;
        len -= 4;
    }
    while (len > 0) {
        h ^= *p++;
        h *= 0x100000001b3ULL;
        --len;
    }
    return h;
}

// Global thread-safe open-addressing interner for deduplicating distinct names
// Fixed capacity 262,144 slots (sufficient for 100,000 distinct names at ~38% load factor)
struct ConcurrentStringInterner {
    static constexpr size_t TABLE_SIZE = 262144;
    static constexpr uint32_t MASK = TABLE_SIZE - 1;

    struct alignas(16) Slot {
        std::atomic<uint64_t> hash{0};
        uint32_t offset{0};
        uint32_t id{0};
        uint16_t len{0};
        std::atomic<uint8_t> state{0}; // 0 = empty, 1 = initializing, 2 = ready
    };

    Slot slots[TABLE_SIZE];
    char pool[4 * 1024 * 1024]; // 4 MB fixed arena
    std::atomic<uint32_t> pool_bump{0};
    std::atomic<uint32_t> next_id{0};

    uint32_t intern(const char* s, size_t len, uint64_t h) {
        if (h == 0) h = 1;
        uint32_t idx = static_cast<uint32_t>(h) & MASK;

        while (true) {
            uint8_t st = slots[idx].state.load(std::memory_order_acquire);
            if (st == 2) {
                if (slots[idx].hash.load(std::memory_order_relaxed) == h && slots[idx].len == len) {
                    if (len == 0 || std::memcmp(pool + slots[idx].offset, s, len) == 0) {
                        return slots[idx].id;
                    }
                }
            } else if (st == 1) {
                // Another thread is actively initializing this slot.
                // We MUST wait for it to become ready (state == 2) rather than skipping it,
                // otherwise we could skip past the very slot where our string is being inserted!
                while ((st = slots[idx].state.load(std::memory_order_acquire)) == 1) {
                    #if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
                    #endif
                }
                if (st == 2) {
                    if (slots[idx].hash.load(std::memory_order_relaxed) == h && slots[idx].len == len) {
                        if (len == 0 || std::memcmp(pool + slots[idx].offset, s, len) == 0) {
                            return slots[idx].id;
                        }
                    }
                }
            } else if (st == 0) {
                uint8_t expected = 0;
                if (slots[idx].state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
                    // Claimed! Fill data
                    uint32_t off = pool_bump.fetch_add(static_cast<uint32_t>(len), std::memory_order_relaxed);
                    if (len > 0) {
                        std::memcpy(pool + off, s, len);
                    }
                    slots[idx].offset = off;
                    slots[idx].len = static_cast<uint16_t>(len);
                    slots[idx].id = next_id.fetch_add(1, std::memory_order_relaxed);
                    slots[idx].hash.store(h, std::memory_order_relaxed);
                    slots[idx].state.store(2, std::memory_order_release);
                    return slots[idx].id;
                }
                continue;
            }
            idx = (idx + 1) & MASK;
        }
    }
};

// Compact flat hash set for interned integer IDs per country
struct FlatIdSet {
    std::vector<uint32_t> table;
    uint32_t mask;
    uint32_t size_count = 0;

    FlatIdSet(size_t initial_cap = 64) {
        table.assign(initial_cap, 0);
        mask = static_cast<uint32_t>(initial_cap - 1);
    }

    inline void insert(uint32_t id) {
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

    inline size_t size() const { return size_count; }
};

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

    void merge(const CountryAgg& other) {
        if (!other.active) return;
        active = true;
        count += other.count;
        id_sum += other.id_sum;
        amount_sum += other.amount_sum;
        if (other.amount_min < amount_min) amount_min = other.amount_min;
        if (other.amount_max > amount_max) amount_max = other.amount_max;
        name_bytes += other.name_bytes;
        note_bytes += other.note_bytes;
        for (uint32_t val : other.distinct_name_ids.table) {
            if (val != 0) {
                distinct_name_ids.insert(val - 1);
            }
        }
    }
};

static constexpr size_t MAX_REGIONS = 1000000;
static constexpr uint16_t UNMATCHED_ID = 0xFFFF;

struct WorkerData {
    std::vector<CountryAgg> local_countries;
    double process_ms = 0;
};

void process_chunk(const char* chunk_start, const char* chunk_end,
                   const uint16_t* region_to_country, uint16_t unmatched_country_id,
                   const std::vector<std::string>& country_names,
                   ConcurrentStringInterner* interner,
                   WorkerData* worker) {
    auto t0 = clock_type::now();
    worker->local_countries.resize(country_names.size());
    for (size_t i = 0; i < country_names.size(); ++i) {
        worker->local_countries[i].name = country_names[i];
        worker->local_countries[i].distinct_name_ids = FlatIdSet(256);
    }

    const char* ptr = chunk_start;
    const char* end = chunk_end;

    char stack_unescape[256];

    while (ptr < end) {
        // Strip BOM
        if (end - ptr >= 3 && static_cast<unsigned char>(ptr[0]) == 0xEF &&
            static_cast<unsigned char>(ptr[1]) == 0xBB && static_cast<unsigned char>(ptr[2]) == 0xBF) {
            ptr += 3;
        }
        if (ptr >= end || *ptr == '\r' || *ptr == '\n') {
            while (ptr < end && (*ptr == '\r' || *ptr == '\n')) ptr++;
            continue;
        }

        // 1. Fast ID parse
        uint64_t record_id = 0;
        while (ptr < end && static_cast<unsigned char>(*ptr - '0') <= 9) {
            record_id = record_id * 10 + (*ptr++ - '0');
        }
        if (ptr < end && *ptr == ',') ptr++;

        // 2. Name parse with in-situ / stack unescaping
        const char* unescaped_name_ptr = "";
        size_t unescaped_name_len = 0;
        uint64_t nh = 0;

        if (ptr < end && *ptr == '"') {
            ptr++;
            const char* p_name_start = ptr;
            uint32_t name_escaped_quotes = 0;

            while (ptr < end) {
                if (*ptr == '"') {
                    if (ptr + 1 < end && *(ptr + 1) == '"') {
                        name_escaped_quotes++;
                        ptr += 2;
                    } else {
                        break;
                    }
                } else {
                    ptr++;
                }
            }
            const char* p_name_end = ptr;
            if (ptr < end && *ptr == '"') ptr++;
            if (ptr < end && *ptr == ',') ptr++;

            size_t raw_name_len = p_name_end - p_name_start;
            unescaped_name_len = raw_name_len - name_escaped_quotes;

            if (name_escaped_quotes == 0) {
                unescaped_name_ptr = p_name_start;
            } else {
                size_t w = 0;
                for (size_t r = 0; r < raw_name_len; ++r) {
                    if (p_name_start[r] == '"' && r + 1 < raw_name_len && p_name_start[r + 1] == '"') {
                        stack_unescape[w++] = '"';
                        ++r;
                    } else {
                        stack_unescape[w++] = p_name_start[r];
                    }
                }
                unescaped_name_ptr = stack_unescape;
            }
            nh = hash_bytes(unescaped_name_ptr, unescaped_name_len);
        } else {
            while (ptr < end && *ptr != ',') ptr++;
            if (ptr < end && *ptr == ',') ptr++;
            unescaped_name_ptr = "";
            unescaped_name_len = 0;
            nh = 0xcbf29ce484222325ULL;
        }

        uint32_t name_id = interner->intern(unescaped_name_ptr, unescaped_name_len, nh);

        // 3. Note parse (unescaped length only)
        size_t unescaped_note_len = 0;
        if (ptr < end && *ptr == '"') {
            ptr++;
            const char* p_note_start = ptr;
            uint32_t note_escaped_quotes = 0;
            while (ptr < end) {
                if (*ptr == '"') {
                    if (ptr + 1 < end && *(ptr + 1) == '"') {
                        note_escaped_quotes++;
                        ptr += 2;
                    } else {
                        break;
                    }
                } else {
                    ptr++;
                }
            }
            const char* p_note_end = ptr;
            if (ptr < end && *ptr == '"') ptr++;
            if (ptr < end && *ptr == ',') ptr++;
            unescaped_note_len = (p_note_end - p_note_start) - note_escaped_quotes;
        } else {
            while (ptr < end && *ptr != ',') ptr++;
            if (ptr < end && *ptr == ',') ptr++;
            unescaped_note_len = 0;
        }

        // 4. Region ID parse (fast integer accumulator)
        uint64_t region_id = 0;
        while (ptr < end && static_cast<unsigned char>(*ptr - '0') <= 9) {
            region_id = region_id * 10 + (*ptr++ - '0');
        }
        if (ptr < end && *ptr == ',') ptr++;

        // 5. Amount parse: [-]WHOLE.FRAC
        bool neg = false;
        if (ptr < end && *ptr == '-') {
            neg = true;
            ptr++;
        }
        int64_t whole = 0;
        while (ptr < end && static_cast<unsigned char>(*ptr - '0') <= 9) {
            whole = whole * 10 + (*ptr++ - '0');
        }
        int64_t frac = 0;
        if (ptr < end && *ptr == '.') {
            ptr++;
            if (ptr < end && static_cast<unsigned char>(*ptr - '0') <= 9) frac += (*ptr++ - '0') * 10;
            if (ptr < end && static_cast<unsigned char>(*ptr - '0') <= 9) frac += (*ptr++ - '0');
        }
        int64_t cents = whole * 100 + frac;
        if (neg) cents = -cents;

        while (ptr < end && (*ptr == '\r' || *ptr == '\n')) ptr++;

        // 6. Direct lookup join & aggregate
        uint16_t cid = (region_id < MAX_REGIONS) ? region_to_country[region_id] : UNMATCHED_ID;
        if (cid == UNMATCHED_ID) cid = unmatched_country_id;

        CountryAgg& c = worker->local_countries[cid];
        c.active = true;
        c.add(record_id, name_id, unescaped_name_len, unescaped_note_len, cents);
    }
    worker->process_ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    auto t_start = clock_type::now();

    std::string data_dir = "/data";
    std::string out_dir = "/out";

    if (argc >= 2) data_dir = argv[1];
    if (argc >= 3) out_dir = argv[2];

    unsigned int num_workers = std::thread::hardware_concurrency();
    if (argc >= 4) {
        try {
            int parsed = std::stoi(argv[3]);
            num_workers = std::max(1, std::min(64, parsed));
        } catch (...) {
            num_workers = num_workers > 0 ? num_workers : 8;
        }
    } else {
        num_workers = num_workers > 0 ? num_workers : 8;
    }

    fs::path regions_path = fs::path(data_dir) / "regions.csv";
    fs::path records_path = fs::path(data_dir) / "records.csv";
    fs::path output_path = fs::path(out_dir) / "aggregates.txt";

    std::vector<uint16_t> region_to_country(MAX_REGIONS, UNMATCHED_ID);
    std::vector<std::string> country_names;
    country_names.reserve(256);

    auto get_country_id = [&](std::string_view cname) -> uint16_t {
        for (uint16_t i = 0; i < country_names.size(); ++i) {
            if (country_names[i] == cname) return i;
        }
        uint16_t new_id = static_cast<uint16_t>(country_names.size());
        country_names.emplace_back(cname);
        return new_id;
    };

    uint16_t unmatched_country_id = get_country_id("_UNMATCHED");

    // Fast regions.csv mmap parse
    int rfd = open(regions_path.c_str(), O_RDONLY);
    if (rfd >= 0) {
        struct stat rsb;
        if (fstat(rfd, &rsb) == 0 && rsb.st_size > 0) {
            const char* rdata = static_cast<const char*>(mmap(nullptr, rsb.st_size, PROT_READ, MAP_PRIVATE, rfd, 0));
            const char* rp = rdata;
            const char* rend = rdata + rsb.st_size;
            while (rp < rend && *rp != '\n') rp++;
            if (rp < rend && *rp == '\n') rp++;
            while (rp < rend) {
                if (*rp == '\r' || *rp == '\n') { rp++; continue; }
                uint64_t rid = 0;
                while (rp < rend && *rp >= '0' && *rp <= '9') rid = rid * 10 + (*rp++ - '0');
                if (rp < rend && *rp == ',') rp++;
                const char* cp_start = rp;
                while (rp < rend && *rp != '\r' && *rp != '\n') rp++;
                std::string_view cname(cp_start, rp - cp_start);
                uint16_t cid = get_country_id(cname);
                if (rid < MAX_REGIONS) {
                    region_to_country[rid] = cid;
                }
                while (rp < rend && (*rp == '\r' || *rp == '\n')) rp++;
            }
            munmap(const_cast<char*>(rdata), rsb.st_size);
        }
        close(rfd);
    }

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

    const char* file_start = data;
    const char* file_end = data + file_size;

    if (file_end - file_start >= 3 && static_cast<unsigned char>(file_start[0]) == 0xEF &&
        static_cast<unsigned char>(file_start[1]) == 0xBB && static_cast<unsigned char>(file_start[2]) == 0xBF) {
        file_start += 3;
    }

    while (file_start < file_end && *file_start != '\n') file_start++;
    if (file_start < file_end && *file_start == '\n') file_start++;

    std::vector<const char*> chunk_starts(num_workers + 1);
    chunk_starts[0] = file_start;
    chunk_starts[num_workers] = file_end;

    size_t content_size = file_end - file_start;
    size_t target_chunk_size = content_size / num_workers;

    for (unsigned int i = 1; i < num_workers; ++i) {
        const char* target = file_start + i * target_chunk_size;
        const char* cand = target;
        while (cand < file_end && *cand != '\n') cand++;
        if (cand < file_end) cand++;

        while (cand < file_end) {
            const char* cp = cand;
            if (file_end - cp >= 3 && static_cast<unsigned char>(cp[0]) == 0xEF &&
                static_cast<unsigned char>(cp[1]) == 0xBB && static_cast<unsigned char>(cp[2]) == 0xBF) {
                cp += 3;
            }
            bool has_id = false;
            while (cp < file_end && *cp >= '0' && *cp <= '9') {
                has_id = true;
                cp++;
            }
            if (has_id && cp < file_end && *cp == ',') {
                cp++;
                if (cp < file_end && (*cp == ',' || *cp == '"')) {
                    break;
                }
            }
            while (cand < file_end && *cand != '\n') cand++;
            if (cand < file_end) cand++;
        }
        chunk_starts[i] = cand;
    }

    // Single shared concurrent interner
    static ConcurrentStringInterner global_interner;

    std::vector<std::thread> workers;
    std::vector<WorkerData> worker_data(num_workers);

    for (unsigned int i = 0; i < num_workers; ++i) {
        if (chunk_starts[i] < chunk_starts[i + 1]) {
            workers.emplace_back(process_chunk, chunk_starts[i], chunk_starts[i + 1],
                                 region_to_country.data(), unmatched_country_id,
                                 std::cref(country_names), &global_interner, &worker_data[i]);
        }
    }

    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    munmap(const_cast<char*>(data), file_size);
    close(fd);

    std::vector<CountryAgg> global_countries(country_names.size());
    for (size_t i = 0; i < country_names.size(); ++i) {
        global_countries[i].name = country_names[i];
    }

    for (const auto& wdata : worker_data) {
        for (size_t c = 0; c < wdata.local_countries.size(); ++c) {
            global_countries[c].merge(wdata.local_countries[c]);
        }
    }

    std::vector<CountryAgg*> active_countries;
    for (CountryAgg& c : global_countries) {
        if (c.active) {
            active_countries.push_back(&c);
        }
    }

    std::sort(active_countries.begin(), active_countries.end(), [](const CountryAgg* a, const CountryAgg* b) {
        return a->name < b->name;
    });

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
    double total_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);

    std::cerr << "C03 EXECUTION RESULT:"
              << " total_ms=" << total_ms
              << " peak_rss_kb=" << usage.ru_maxrss
              << " workers=" << num_workers
              << " distinct_names=" << global_interner.next_id.load()
              << "\n";

    return 0;
}
