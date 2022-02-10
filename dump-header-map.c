//cc -Wextra -o build/dump-header-map dump-header-map.c
// see: https://github.com/llvm-mirror/clang/blob/release_40/include/clang/Lex/HeaderMapTypes.h
// https://github.com/llvm-mirror/clang/blob/release_40/include/clang/Lex/HeaderMap.h// https://github.com/llvm-mirror/clang/blob/release_40/lib/Lex/HeaderMap.cpp

// This is basically dump() from there.

#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <err.h>
#include <fcntl.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>

enum {
  HMAP_HeaderMagicNumber = ('h' << 24) | ('m' << 16) | ('a' << 8) | 'p',
  HMAP_HeaderVersion = 0x0001,
  HMAP_EmptyBucketKey = 0,

  HMAP_SwappedMagic = ('h' << 0) | ('m' << 8) | ('a' << 16) | ('p' << 24),
  HMAP_SwappedVersion = 0x0100,
};

struct HMapBucket {
  uint32_t Key;    // Offset (into strings) of key.
  uint32_t Prefix; // Offset (into strings) of value prefix.
  uint32_t Suffix; // Offset (into strings) of value suffix.
};

struct HMapHeader {
  uint32_t Magic;          // Magic word, also indicates byte order.
  uint16_t Version;        // Version number -- currently 1.
  uint16_t Reserved;       // Reserved for future use - zero for now.
  uint32_t StringsOffset;  // Offset to start of string pool.
  uint32_t NumEntries;     // Number of entries in the string table.
  uint32_t NumBuckets;     // Number of buckets (always a power of 2).
  uint32_t MaxValueLength; // Length of longest result path (excluding nul).
  // An array of 'NumBuckets' HMapBucket objects follows this header.
  // Strings follow the buckets, at StringsOffset.
};

uint32_t nop_swap(uint32_t);
uint32_t actually_swap(uint32_t);
void dump(const char *path);

int
main(int argc, const char *argv[])
{
    assert(actually_swap(HMAP_HeaderMagicNumber) == HMAP_SwappedMagic
        && "failed to properly swap things :(");

    if (argc < 2) {
        fprintf(
            stderr,
            "usage: %s HMAP_FILE [HMAP_FILE...]\n\n"
            "Dump clang headermap (.hmap file) contents.\n\n"
            "See: https://github.com/llvm-mirror/clang/blob/release_40/include/clang/Lex/HeaderMapTypes.h\n"
            "and related files\n",
            getprogname());
        return EX_USAGE;
    }

    for (int i = 1; i < argc; ++i) {
        dump(argv[i]);
        putchar('\n');
    }
    return EXIT_SUCCESS;
}

void
dump(const char *path)
{
    int fd = open(path, O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        warn(/*EX_NOINPUT,*/ "%s: cannot open", path);
        return;
    }

    struct HMapHeader header;
    ssize_t nread = read(fd, &header, sizeof(header));
    if (nread < 0) {
        warn(/*EX_IOERR,*/ "%s: failed to read header", path);
        (void)close(fd);
        return;
    } else if ((size_t)nread < sizeof(header)) {
        warn(
            /*EX_DATAERR,*/
            "%s: short read: expected %zu bytes, read only %zd",
            path, sizeof(header), nread);
        (void)close(fd);
        return;
    }

    bool is_swapped = false;
    uint32_t (*swap)(uint32_t) = nop_swap;
    if (header.Magic == HMAP_HeaderMagicNumber
        && header.Version == HMAP_HeaderVersion) {
        is_swapped = false;
    } else if (header.Magic == HMAP_SwappedMagic
        && header.Version == HMAP_SwappedVersion) {
        is_swapped = true;
        swap = actually_swap;
    } else {
        warn(/*EX_DATAERR,*/ "header lacks HMAP magic");
        (void)close(fd);
        return;
    }

    const uint32_t bucket_count = swap(header.NumBuckets);
    printf("Header map: %s\n"
        "\tHash bucket count: %" PRIu32 "\n"
        "\tString table entry count: %" PRIu32 "\n"
        "\tMax value length: %" PRIu32 " bytes\n",
        path,
        bucket_count,
        swap(header.NumEntries),
        swap(header.MaxValueLength));

    struct stat stat;
    int stat_err = fstat(fd, &stat);
    if (stat_err) {
        warn("%s: fstat failed; cannot dump buckets", path);
        (void)close(fd);
        return;
    }

    off_t hmap_length = stat.st_size;
    const void *hmap = mmap(0, hmap_length, PROT_READ, MAP_FILE|MAP_PRIVATE, fd, 0 /*offset*/);
    (void)close(fd);
    if (MAP_FAILED == hmap) {
        warn("%s: failed to mmap; cannot dump buckets", path);
        return;
    }

    const char *raw = hmap;
    const struct HMapBucket *const buckets = (void *)(raw
        + sizeof(struct HMapHeader));
    const char *const string_table = (raw
        + sizeof(struct HMapHeader)
        + bucket_count*sizeof(struct HMapBucket));
    for (uint32_t i = 0; i < bucket_count; ++i) {
        const struct HMapBucket *const bucket = &buckets[i];
        if (swap(bucket->Key) == HMAP_EmptyBucketKey) { continue; }

        // LLVM is careful to sanity-check bucket-count and strlen,
        // but we're just going to assume they're all NUL-terminated
        // and not maliciously crafted input files.
        //
        // This is naive, but this is also not exactly "production" code.
        const char *key = string_table + swap(bucket->Key);
        const char *prefix = string_table + swap(bucket->Prefix);
        const char *suffix = string_table + swap(bucket->Suffix);

        printf("\t- Bucket %" PRIu32 ": "
            "Key %s -> Prefix %s, Suffix %s\n",
            i,
            key, prefix, suffix);
    }
}


uint32_t
nop_swap(uint32_t u)
{
    return u;
}

uint32_t
actually_swap(uint32_t u)
{
    uint32_t n = (
        ((u & 0xFF) << 24)
        | ((u & 0xFF00) << 8)
        | ((u & 0xFF0000) >> 8)
        | ((u & 0xFF000000) >> 24)
        );
    return n;
}
