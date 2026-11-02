#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct Term { std::string word; std::vector<uint32_t> docs; };
struct Query { uint8_t kind = 0, n = 0; std::array<int32_t, 8> t{}; std::array<uint8_t, 7> op{}; };
struct Span { const char *p; uint32_t n; };

static inline bool alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
static inline unsigned char lower_ascii(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static uint64_t hash_word(const char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) h = (h ^ lower_ascii((unsigned char)p[i])) * 1099511628211ULL;
    return h;
}
static void append_num(std::string &s, uint64_t x) {
    char b[32]; auto r = std::to_chars(b, b + sizeof(b), x); s.append(b, r.ptr);
}

struct Corpus {
    int fd = -1; const char *data = nullptr; size_t size = 0;
    explicit Corpus(const std::string &path) {
        fd = open(path.c_str(), O_RDONLY); if (fd < 0) throw std::runtime_error("open docs");
        struct stat st{}; if (fstat(fd, &st) || st.st_size <= 0) throw std::runtime_error("stat docs");
        size = (size_t)st.st_size; data = (const char *)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) throw std::runtime_error("mmap docs");
    }
    ~Corpus() { if (data && data != MAP_FAILED) munmap((void *)data, size); if (fd >= 0) close(fd); }
};

static int lookup(const char *p, size_t n, uint64_t h, const std::vector<int32_t> &slots,
                  const std::vector<Term> &terms) {
    size_t m = slots.size() - 1, k = (size_t)h & m;
    for (;;) {
        int32_t v = slots[k]; if (!v) return -1;
        const std::string &w = terms[(size_t)v - 1].word;
        if (w.size() == n) {
            size_t i = 0; while (i < n && (unsigned char)w[i] == lower_ascii((unsigned char)p[i])) ++i;
            if (i == n) return v - 1;
        }
        k = (k + 1) & m;
    }
}

static std::string format_ids(const std::vector<uint32_t> &ids) {
    std::string s; s.reserve(64); append_num(s, ids.size()); s.push_back(' ');
    uint64_t sum = 0; for (uint32_t x : ids) sum += x;
    append_num(s, sum);
    for (size_t i = 0; i < ids.size() && i < 10; ++i) { s.push_back(' '); append_num(s, ids[i]); }
    s.push_back('\n'); return s;
}

static std::vector<uint32_t> combine(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, uint8_t op) {
    std::vector<uint32_t> out;
    if (op == 0) { // AND; gallop through the smaller side when sizes are skewed.
        const auto *small = &a, *large = &b;
        if (small->size() > large->size()) std::swap(small, large);
        out.reserve(small->size());
        if (large->size() > small->size() * 16ULL + 256) {
            auto it = large->begin();
            for (uint32_t x : *small) {
                it = std::lower_bound(it, large->end(), x);
                if (it == large->end()) break;
                if (*it == x) { out.push_back(x); ++it; }
            }
        } else {
            size_t i = 0, j = 0;
            while (i < a.size() && j < b.size()) {
                if (a[i] == b[j]) { out.push_back(a[i]); ++i; ++j; }
                else if (a[i] < b[j]) ++i; else ++j;
            }
        }
    } else if (op == 1) { // OR
        out.reserve(a.size() + b.size()); size_t i = 0, j = 0;
        while (i < a.size() || j < b.size()) {
            uint32_t x = j == b.size() || (i < a.size() && a[i] < b[j]) ? a[i++] :
                         i == a.size() || b[j] < a[i] ? b[j++] : (uint32_t)(++i, ++j, a[i - 1]);
            out.push_back(x);
        }
    } else { // NOT means set difference: a \ b
        out.reserve(a.size());
        if (b.size() * 16ULL + 256 < a.size()) {
            size_t j = 0;
            for (uint32_t x : a) {
                while (j < b.size() && b[j] < x) ++j;
                if (j == b.size() || b[j] != x) out.push_back(x);
            }
        } else {
            size_t i = 0, j = 0;
            while (i < a.size()) {
                while (j < b.size() && b[j] < a[i]) ++j;
                if (j == b.size() || b[j] != a[i]) out.push_back(a[i]);
                ++i;
            }
        }
    }
    return out;
}

static bool phrase_match(const Span &d, const Query &q, const std::vector<int32_t> &slots,
                         const std::vector<Term> &terms) {
    if (q.n == 0 || d.n == 0) return false;
    std::array<int32_t, 8> ring{}; size_t used = 0, pos = 0, i = 0;
    while (i < d.n) {
        while (i < d.n && !alnum((unsigned char)d.p[i])) ++i;
        size_t start = i; uint64_t h = 1469598103934665603ULL;
        while (i < d.n && alnum((unsigned char)d.p[i])) h = (h ^ lower_ascii((unsigned char)d.p[i++])) * 1099511628211ULL;
        if (i == start) continue;
        ring[pos] = lookup(d.p + start, i - start, h, slots, terms); pos = (pos + 1) % q.n;
        if (used < q.n) ++used;
        if (used == q.n) {
            size_t j = 0; while (j < q.n && ring[(pos + j) % q.n] == q.t[j]) ++j;
            if (j == q.n) return true;
        }
    }
    return false;
}

int main(int argc, char **argv) try {
    std::string data_dir = argc > 1 ? argv[1] : "/data", out_dir = argc > 2 ? argv[2] : "/out";
    std::string query_text; { std::ifstream f(data_dir + "/queries.txt", std::ios::binary);
        if (!f) throw std::runtime_error("open queries"); query_text.assign(std::istreambuf_iterator<char>(f), {}); }
    std::vector<Term> terms; terms.reserve(30000);
    std::unordered_map<std::string, int32_t> intern; intern.reserve(30000);
    auto term_id = [&](const char *p, size_t n) {
        std::string w; w.resize(n); for (size_t i = 0; i < n; ++i) w[i] = (char)lower_ascii((unsigned char)p[i]);
        auto it = intern.find(w); if (it != intern.end()) return it->second;
        int32_t id = (int32_t)terms.size(); terms.push_back({w, {}}); intern.emplace(std::move(w), id); return id;
    };
    std::vector<Query> queries; queries.reserve(50000);
    for (size_t p = 0; p < query_text.size();) {
        size_t e = query_text.find('\n', p); if (e == std::string::npos) e = query_text.size();
        const char *line = query_text.data() + p; size_t len = e - p; Query q;
        if (len >= 6 && std::memcmp(line, "BOOL ", 5) == 0) {
            q.kind = 0; size_t x = 5; bool expect_term = true;
            while (x < len) {
                while (x < len && line[x] == ' ') ++x; if (x == len) break;
                size_t a = x; while (x < len && line[x] != ' ') ++x; size_t n = x - a;
                if (expect_term) { if (q.n >= 8) throw std::runtime_error("too many terms"); q.t[q.n++] = term_id(line + a, n); }
                else {
                    uint8_t op = n == 3 && !std::memcmp(line + a, "AND", 3) ? 0 : n == 2 && !std::memcmp(line + a, "OR", 2) ? 1 : n == 3 && !std::memcmp(line + a, "NOT", 3) ? 2 : 255;
                    if (op == 255) throw std::runtime_error("bad boolean operator"); q.op[q.n - 1] = op;
                }
                expect_term = !expect_term;
            }
        } else if (len >= 9 && std::memcmp(line, "PHRASE \"", 8) == 0) {
            q.kind = 1; size_t x = 8, end = len && line[len - 1] == '"' ? len - 1 : len;
            while (x < end) { while (x < end && line[x] == ' ') ++x; size_t a = x; while (x < end && line[x] != ' ') ++x;
                if (a < x) { if (q.n >= 8) throw std::runtime_error("phrase too long"); q.t[q.n++] = term_id(line + a, x - a); } }
        } else throw std::runtime_error("bad query");
        if (!q.n) throw std::runtime_error("empty query"); queries.push_back(q); p = e + 1;
    }
    intern.clear(); intern.rehash(0); query_text.clear(); query_text.shrink_to_fit();

    size_t cap = 1; while (cap < terms.size() * 2) cap <<= 1;
    std::vector<int32_t> slots(cap, 0);
    for (size_t id = 0; id < terms.size(); ++id) {
        const std::string &w = terms[id].word; size_t k = (size_t)hash_word(w.data(), w.size()) & (cap - 1);
        while (slots[k]) k = (k + 1) & (cap - 1); slots[k] = (int32_t)id + 1;
    }
    std::vector<Span> docs; docs.reserve(250000);
    Corpus corpus(data_dir + "/docs.txt"); size_t pos = 0;
    while (pos < corpus.size) {
        size_t end = pos; while (end < corpus.size && corpus.data[end] != '\n') ++end;
        size_t tab = pos; while (tab < end && corpus.data[tab] != '\t') ++tab;
        if (tab == end) throw std::runtime_error("bad document line");
        docs.push_back({corpus.data + tab + 1, (uint32_t)(end - tab - 1)});
        pos = end + (end < corpus.size);
    }

    std::vector<std::string> answers(queries.size()); std::atomic<size_t> next{0};
    unsigned workers = std::thread::hardware_concurrency(); if (!workers) workers = 4; workers = std::min(workers, 12u);
    {
        std::vector<std::vector<std::vector<uint32_t>>> local; local.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) local.emplace_back(terms.size());
        std::vector<std::thread> index_pool; index_pool.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) {
            size_t first = docs.size() * w / workers, last_doc = docs.size() * (w + 1) / workers;
            index_pool.emplace_back([&, w, first, last_doc] {
                std::vector<uint32_t> last(terms.size(), UINT32_MAX);
                for (size_t d = first; d < last_doc; ++d) {
                    const Span &span = docs[d]; size_t i = 0;
                    while (i < span.n) {
                        while (i < span.n && !alnum((unsigned char)span.p[i])) ++i;
                        size_t a = i; uint64_t h = 1469598103934665603ULL;
                        while (i < span.n && alnum((unsigned char)span.p[i])) h = (h ^ lower_ascii((unsigned char)span.p[i++])) * 1099511628211ULL;
                        if (i == a) continue;
                        int id = lookup(span.p + a, i - a, h, slots, terms);
                        if (id >= 0 && last[id] != d) { last[id] = (uint32_t)d; local[w][id].push_back((uint32_t)d); }
                    }
                }
            });
        }
        for (auto &t : index_pool) t.join();
        for (size_t id = 0; id < terms.size(); ++id) {
            size_t total = 0; for (unsigned w = 0; w < workers; ++w) total += local[w][id].size();
            auto &dest = terms[id].docs; dest.reserve(total);
            for (unsigned w = 0; w < workers; ++w) dest.insert(dest.end(), local[w][id].begin(), local[w][id].end());
        }
    }
    auto work = [&]() {
        std::vector<uint32_t> cur;
        for (;;) {
            size_t qi = next.fetch_add(16, std::memory_order_relaxed); if (qi >= queries.size()) break;
            size_t stop = std::min(qi + 16, queries.size());
            for (; qi < stop; ++qi) {
                const Query &q = queries[qi];
                if (q.kind == 0) {
                    cur = terms[q.t[0]].docs;
                    for (size_t j = 1; j < q.n; ++j) cur = combine(cur, terms[q.t[j]].docs, q.op[j - 1]);
                    answers[qi] = format_ids(cur);
                } else {
                    int32_t rare = q.t[0];
                    for (size_t j = 1; j < q.n; ++j) if (terms[q.t[j]].docs.size() < terms[rare].docs.size()) rare = q.t[j];
                    cur.clear(); const auto &cand = terms[rare].docs; cur.reserve(cand.size());
                    for (uint32_t d : cand) if (phrase_match(docs[d], q, slots, terms)) cur.push_back(d);
                    answers[qi] = format_ids(cur);
                }
            }
        }
    };
    std::vector<std::thread> pool; pool.reserve(workers); for (unsigned i = 0; i < workers; ++i) pool.emplace_back(work);
    for (auto &t : pool) t.join();
    std::ofstream out(out_dir + "/results.txt", std::ios::binary); if (!out) throw std::runtime_error("open output");
    for (const auto &s : answers) out.write(s.data(), (std::streamsize)s.size());
    return 0;
} catch (const std::exception &e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
