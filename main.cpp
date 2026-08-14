#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace {
constexpr uint64_t MAGIC = 0x32564244454c4946ULL;
constexpr uint32_t VERSION = 2;
constexpr uint32_t BUCKET_COUNT = 1u << 17;
constexpr const char *DATABASE_FILE = "file_storage.db";

enum IndexKind { PAIR_INDEX = 0, KEY_INDEX = 1 };

struct Header {
    uint64_t magic;
    uint32_t version;
    uint32_t buckets;
    uint32_t node_count;
    uint32_t free_head;
};

struct Node {
    uint32_t pair_next, pair_prev;
    uint32_t key_next, key_prev;
    int32_t value;
    char key[65];
};

class Database {
    FILE *file_ = nullptr;
    Header header_{};

    static uint64_t hash_key(const char *s) {
        uint64_t h = 1469598103934665603ULL;
        while (*s) {
            h ^= static_cast<unsigned char>(*s++);
            h *= 1099511628211ULL;
        }
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return h;
    }

    static uint64_t pair_hash(uint64_t key_hash, uint32_t value) {
        uint64_t x = key_hash ^ (static_cast<uint64_t>(value) + 0x9e3779b97f4a7c15ULL +
                                 (key_hash << 6) + (key_hash >> 2));
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return x;
    }

    static long bucket_position(IndexKind kind, uint32_t bucket) {
        return static_cast<long>(sizeof(Header)) +
               (static_cast<long>(kind) * BUCKET_COUNT + bucket) * sizeof(uint32_t);
    }

    static long node_position(uint32_t id) {
        return static_cast<long>(sizeof(Header)) +
               static_cast<long>(2 * BUCKET_COUNT) * sizeof(uint32_t) +
               static_cast<long>(id - 1) * sizeof(Node);
    }

    uint32_t read_bucket(IndexKind kind, uint32_t bucket) {
        uint32_t head = 0;
        std::fseek(file_, bucket_position(kind, bucket), SEEK_SET);
        std::fread(&head, sizeof(head), 1, file_);
        return head;
    }

    void write_bucket(IndexKind kind, uint32_t bucket, uint32_t head) {
        std::fseek(file_, bucket_position(kind, bucket), SEEK_SET);
        std::fwrite(&head, sizeof(head), 1, file_);
    }

    Node read_node(uint32_t id) {
        Node node{};
        std::fseek(file_, node_position(id), SEEK_SET);
        std::fread(&node, sizeof(node), 1, file_);
        return node;
    }

    void write_node(uint32_t id, const Node &node) {
        std::fseek(file_, node_position(id), SEEK_SET);
        std::fwrite(&node, sizeof(node), 1, file_);
    }

    void initialize() {
        header_ = {MAGIC, VERSION, BUCKET_COUNT, 0, 0};
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(&header_, sizeof(header_), 1, file_);
        uint32_t zeros[4096]{};
        for (uint32_t written = 0; written < 2 * BUCKET_COUNT; written += 4096)
            std::fwrite(zeros, sizeof(uint32_t), 4096, file_);
        std::fflush(file_);
    }

public:
    Database() {
        file_ = std::fopen(DATABASE_FILE, "r+b");
        if (!file_) {
            file_ = std::fopen(DATABASE_FILE, "w+b");
            initialize();
        } else if (std::fread(&header_, sizeof(header_), 1, file_) != 1 ||
                   header_.magic != MAGIC || header_.version != VERSION ||
                   header_.buckets != BUCKET_COUNT) {
            std::fclose(file_);
            file_ = std::fopen(DATABASE_FILE, "w+b");
            initialize();
        }
    }

    ~Database() {
        if (file_) {
            std::fseek(file_, 0, SEEK_SET);
            std::fwrite(&header_, sizeof(header_), 1, file_);
            std::fclose(file_);
        }
    }

    void insert(const char *key, int32_t value) {
        const uint64_t kh = hash_key(key);
        const uint32_t pb = static_cast<uint32_t>(pair_hash(kh, value)) & (BUCKET_COUNT - 1);
        const uint32_t kb = static_cast<uint32_t>(kh) & (BUCKET_COUNT - 1);
        const uint32_t pair_head = read_bucket(PAIR_INDEX, pb);
        const uint32_t key_head = read_bucket(KEY_INDEX, kb);

        uint32_t id;
        if (header_.free_head) {
            id = header_.free_head;
            Node free_node = read_node(id);
            header_.free_head = free_node.pair_next;
        } else {
            id = ++header_.node_count;
        }

        Node node{};
        node.pair_next = pair_head;
        node.key_next = key_head;
        node.value = value;
        std::memcpy(node.key, key, std::strlen(key) + 1);
        write_node(id, node);

        if (pair_head) {
            Node old = read_node(pair_head);
            old.pair_prev = id;
            write_node(pair_head, old);
        }
        if (key_head) {
            Node old = read_node(key_head);
            old.key_prev = id;
            write_node(key_head, old);
        }
        write_bucket(PAIR_INDEX, pb, id);
        write_bucket(KEY_INDEX, kb, id);
    }

    void erase(const char *key, int32_t value) {
        const uint64_t kh = hash_key(key);
        const uint32_t pb = static_cast<uint32_t>(pair_hash(kh, value)) & (BUCKET_COUNT - 1);
        uint32_t id = read_bucket(PAIR_INDEX, pb);
        Node node{};
        while (id) {
            node = read_node(id);
            if (node.value == value && std::strcmp(node.key, key) == 0) break;
            id = node.pair_next;
        }
        if (!id) return;

        if (node.pair_prev) {
            Node prev = read_node(node.pair_prev);
            prev.pair_next = node.pair_next;
            write_node(node.pair_prev, prev);
        } else {
            write_bucket(PAIR_INDEX, pb, node.pair_next);
        }
        if (node.pair_next) {
            Node next = read_node(node.pair_next);
            next.pair_prev = node.pair_prev;
            write_node(node.pair_next, next);
        }

        const uint32_t kb = static_cast<uint32_t>(kh) & (BUCKET_COUNT - 1);
        if (node.key_prev) {
            Node prev = read_node(node.key_prev);
            prev.key_next = node.key_next;
            write_node(node.key_prev, prev);
        } else {
            write_bucket(KEY_INDEX, kb, node.key_next);
        }
        if (node.key_next) {
            Node next = read_node(node.key_next);
            next.key_prev = node.key_prev;
            write_node(node.key_next, next);
        }

        node.pair_next = header_.free_head;
        header_.free_head = id;
        write_node(id, node);
    }

    void find(const char *key) {
        const uint64_t kh = hash_key(key);
        uint32_t id = read_bucket(KEY_INDEX, static_cast<uint32_t>(kh) & (BUCKET_COUNT - 1));
        std::vector<int32_t> values;
        while (id) {
            Node node = read_node(id);
            if (std::strcmp(node.key, key) == 0) values.push_back(node.value);
            id = node.key_next;
        }
        if (values.empty()) {
            std::puts("null");
            return;
        }
        std::sort(values.begin(), values.end());
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) std::putchar(' ');
            std::printf("%d", values[i]);
        }
        std::putchar('\n');
    }
};
} // namespace

int main() {
    int n;
    if (std::scanf("%d", &n) != 1) return 0;
    Database database;
    char command[8], key[65];
    int value;
    for (int i = 0; i < n; ++i) {
        if (std::scanf("%7s %64s", command, key) != 2) return 0;
        if (command[0] == 'i') {
            if (std::scanf("%d", &value) != 1) return 0;
            database.insert(key, value);
        } else if (command[0] == 'd') {
            if (std::scanf("%d", &value) != 1) return 0;
            database.erase(key, value);
        } else {
            database.find(key);
        }
    }
    return 0;
}
