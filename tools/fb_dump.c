/*
 * fb_dump: copy the latest frame out of a running core's frame ring and
 * write it as a picture (beads-0ir.3).
 *
 * The ring is the shared-memory region every core publishes the machine's
 * canonical frame to (docs/protocol/fb-ring-v1.md). This program follows
 * that document and nothing else: it includes no header of the emulator, so
 * it is also the proof that a reader in another language can be written from
 * the document alone. Standard C plus the platform's shared memory
 * (shm_open + mmap, or OpenFileMappingA + MapViewOfFile).
 *
 * Usage: fb_dump [-1] [-q] <ring> <out.png | out.ppm>
 *   <ring>  the ring's name as KCPC FB_INFO reports it ("/kcpc-fb-1234" on
 *           POSIX, "Local\kcpc-fb-1234" on Windows), or the core's pid
 *   -1      one picture row per frame row (768x272); by default each row is
 *           written twice, 768x544, as the core's `screenshot` does
 *   -q      print nothing on success
 *
 * On success it prints one line, `seq=N crc32=XXXXXXXX width=W height=H`:
 * the frame's sequence number in the ring and the CRC32 of its pixels, the
 * same CRC32 as the text IPC's `hash frame` of that frame. Exit status 0 on
 * success, 1 on a usage error, 2 when the ring cannot be opened or read, 3
 * when the picture cannot be written.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* shm_open, nanosleep under -std=c11 */
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

/* The header, docs/protocol/fb-ring-v1.md "Layout". Offsets in bytes. */
enum {
  kOffMagic = 0,
  kOffVersion = 4,
  kOffFormat = 12,
  kOffWidth = 16,
  kOffHeight = 20,
  kOffStride = 24,
  kOffRingSlots = 28,
  kOffSlotBytes = 32,
  kOffSlotStride = 40,
  kOffSlotsOffset = 48,
  kOffLatestSeq = 64,
  kOffLatestSlot = 72,
  kOffSlotSeq = 80,
  kHeaderBytes = 256
};
static const uint32_t kMagic = 0x46554246u; /* "FBUF" read little-endian */
static const uint32_t kVersion = 1;
static const uint32_t kFormatRgb24 = 1;
static const uint32_t kMaxSlots = 18; /* what fits before the reserved end */

/* --- acquire loads --------------------------------------------------------
 * The words the producer changes while readers read are 64-bit atomics it
 * stores with release; a reader loads them with acquire (the document's
 * "Reading" steps), which a weakly ordered CPU (ARM) needs. On 32-bit x86 a
 * plain 64-bit load is two loads; both compilers below make it one. */
#if defined(_MSC_VER)
static uint64_t load_acquire_u64(const volatile void* p) {
  uint64_t const v =
      (uint64_t)__iso_volatile_load64((const volatile __int64*)p);
  MemoryBarrier();
  return v;
}
static uint32_t load_acquire_u32(const volatile void* p) {
  uint32_t const v =
      (uint32_t)__iso_volatile_load32((const volatile __int32*)p);
  MemoryBarrier();
  return v;
}
static void fence_acquire(void) { MemoryBarrier(); }
#else
static uint64_t load_acquire_u64(const volatile void* p) {
  return __atomic_load_n((const volatile uint64_t*)p, __ATOMIC_ACQUIRE);
}
static uint32_t load_acquire_u32(const volatile void* p) {
  return __atomic_load_n((const volatile uint32_t*)p, __ATOMIC_ACQUIRE);
}
static void fence_acquire(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
#endif

/* The fields written once, before the magic: plain little-endian reads. */
static uint32_t get_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t* p) {
  return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
#endif
}

/* --- the mapping ---------------------------------------------------------- */

typedef struct {
  const uint8_t* base;
  size_t size;
#ifdef _WIN32
  HANDLE mapping;
#endif
} Ring;

static int ring_open(Ring* ring, const char* name) {
  memset(ring, 0, sizeof(*ring));
#ifdef _WIN32
  MEMORY_BASIC_INFORMATION info;
  ring->mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
  if (ring->mapping == NULL) {
    fprintf(stderr, "fb_dump: no ring named %s (error %lu)\n", name,
            (unsigned long)GetLastError());
    return 0;
  }
  ring->base =
      (const uint8_t*)MapViewOfFile(ring->mapping, FILE_MAP_READ, 0, 0, 0);
  if (ring->base == NULL ||
      VirtualQuery(ring->base, &info, sizeof(info)) == 0) {
    fprintf(stderr, "fb_dump: cannot map %s (error %lu)\n", name,
            (unsigned long)GetLastError());
    if (ring->base != NULL) UnmapViewOfFile(ring->base);
    CloseHandle(ring->mapping);
    return 0;
  }
  ring->size = info.RegionSize;
#else
  struct stat st;
  void* p;
  int const fd = shm_open(name, O_RDONLY, 0);
  if (fd < 0) {
    perror("fb_dump: shm_open");
    fprintf(stderr, "fb_dump: no ring named %s\n", name);
    return 0;
  }
  if (fstat(fd, &st) != 0 || st.st_size < kHeaderBytes) {
    fprintf(stderr, "fb_dump: %s is not a frame ring\n", name);
    close(fd);
    return 0;
  }
  p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    perror("fb_dump: mmap");
    return 0;
  }
  ring->base = (const uint8_t*)p;
  ring->size = (size_t)st.st_size;
#endif
  return 1;
}

static void ring_close(Ring* ring) {
  if (ring->base == NULL) return;
#ifdef _WIN32
  UnmapViewOfFile(ring->base);
  CloseHandle(ring->mapping);
#else
  munmap((void*)ring->base, ring->size);
#endif
  ring->base = NULL;
}

/* --- reading, steps 1 to 5 of the document -------------------------------- */

typedef struct {
  uint32_t width, height, stride, slots;
  uint64_t slot_bytes, slot_stride, slots_offset;
} Layout;

static int read_layout(const Ring* ring, Layout* l) {
  const uint8_t* h = ring->base;
  if (load_acquire_u32(h + kOffMagic) != kMagic) return 0; /* step 1 */
  if (get_u32(h + kOffVersion) != kVersion ||
      get_u32(h + kOffFormat) != kFormatRgb24)
    return 0;
  l->width = get_u32(h + kOffWidth);
  l->height = get_u32(h + kOffHeight);
  l->stride = get_u32(h + kOffStride);
  l->slots = get_u32(h + kOffRingSlots);
  l->slot_bytes = get_u64(h + kOffSlotBytes);
  l->slot_stride = get_u64(h + kOffSlotStride);
  l->slots_offset = get_u64(h + kOffSlotsOffset);
  if (l->slots == 0 || l->slots > kMaxSlots || l->width == 0 ||
      l->height == 0 || l->stride < (uint64_t)l->width * 3 ||
      l->slot_bytes != (uint64_t)l->stride * l->height ||
      l->slot_stride < l->slot_bytes || l->slots_offset < kHeaderBytes)
    return 0;
  if (l->slots_offset + (uint64_t)l->slots * l->slot_stride > ring->size)
    return 0;
  return 1;
}

/* 1: dst holds frame *seq, whole. 0: nothing published yet. -1: the
 * producer lapped every attempt. */
static int read_latest(const Ring* ring, const Layout* l, uint8_t* dst,
                       uint64_t* seq) {
  const uint8_t* h = ring->base;
  int attempt;
  for (attempt = 0; attempt < 64; ++attempt) {
    uint64_t slot, s1, s2;
    if (load_acquire_u64(h + kOffLatestSeq) == 0) return 0; /* step 2 */
    slot = load_acquire_u64(h + kOffLatestSlot);            /* step 3 */
    if (slot >= l->slots) continue;
    s1 = load_acquire_u64(h + kOffSlotSeq + 8 * slot);
    if (s1 == 0 || (s1 & 1) != 0) continue;
    memcpy(dst, h + l->slots_offset + slot * l->slot_stride, /* step 4 */
           (size_t)l->slot_bytes);
    fence_acquire(); /* step 5 */
    s2 = load_acquire_u64(h + kOffSlotSeq + 8 * slot);
    if (s2 != s1) continue;
    *seq = s1 / 2;
    return 1;
  }
  return -1;
}

/* --- CRC32 (zlib's: reflected 0xEDB88320), for the report and for PNG ----- */

static uint32_t crc_table[256];

static void crc_init(void) {
  uint32_t n;
  for (n = 0; n < 256; ++n) {
    uint32_t c = n;
    int k;
    for (k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}

static uint32_t crc_update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

/* --- writing -------------------------------------------------------------- */

typedef struct {
  FILE* f;
  int ok;
  uint32_t crc;   /* of the PNG chunk being written */
  uint32_t adler; /* zlib's Adler-32 of the image data */
} Out;

static void put(Out* o, const void* p, size_t n) {
  if (o->ok && fwrite(p, 1, n, o->f) != n) o->ok = 0;
}

static void put_be32(Out* o, uint32_t v) {
  uint8_t b[4];
  b[0] = (uint8_t)(v >> 24);
  b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);
  b[3] = (uint8_t)v;
  put(o, b, 4);
}

/* Chunk bytes after the length: counted into the chunk's CRC. */
static void chunk_put(Out* o, const void* p, size_t n) {
  o->crc = crc_update(o->crc, (const uint8_t*)p, n);
  put(o, p, n);
}

static void chunk_begin(Out* o, uint32_t length, const char* type) {
  put_be32(o, length);
  o->crc = 0;
  chunk_put(o, type, 4);
}

static void chunk_end(Out* o) { put_be32(o, o->crc); }

static void adler_update(Out* o, const uint8_t* p, size_t n) {
  uint32_t a = o->adler & 0xFFFF, b = o->adler >> 16;
  while (n--) {
    a = (a + *p++) % 65521u;
    b = (b + a) % 65521u;
  }
  o->adler = (b << 16) | a;
}

/* A PNG with the image data in stored (uncompressed) deflate blocks: no
 * zlib needed, and the file is the frame's size plus a little. One scanline
 * (filter byte 0, then the row) per stored block. */
static int write_png(FILE* f, const Layout* l, const uint8_t* px, int rows) {
  static const uint8_t kSignature[8] = {0x89, 'P',  'N',  'G',
                                        '\r', '\n', 0x1A, '\n'};
  Out o;
  uint32_t const row_bytes = l->width * 3;
  uint32_t const line = row_bytes + 1; /* filter byte + row */
  uint32_t const lines = l->height * (uint32_t)rows;
  uint32_t const idat = 2 + lines * (5 + line) + 4;
  uint8_t ihdr[13];
  uint32_t y;
  int r;
  if (line > 0xFFFF) return 0; /* one stored block holds 65535 bytes */
  o.f = f;
  o.ok = 1;
  o.adler = 1;
  put(&o, kSignature, sizeof(kSignature));

  chunk_begin(&o, 13, "IHDR");
  ihdr[0] = (uint8_t)(l->width >> 24);
  ihdr[1] = (uint8_t)(l->width >> 16);
  ihdr[2] = (uint8_t)(l->width >> 8);
  ihdr[3] = (uint8_t)l->width;
  ihdr[4] = (uint8_t)(lines >> 24);
  ihdr[5] = (uint8_t)(lines >> 16);
  ihdr[6] = (uint8_t)(lines >> 8);
  ihdr[7] = (uint8_t)lines;
  ihdr[8] = 8;  /* bits per channel */
  ihdr[9] = 2;  /* truecolour, RGB */
  ihdr[10] = 0; /* deflate */
  ihdr[11] = 0; /* adaptive filtering */
  ihdr[12] = 0; /* no interlace */
  chunk_put(&o, ihdr, sizeof(ihdr));
  chunk_end(&o);

  chunk_begin(&o, idat, "IDAT");
  {
    static const uint8_t kZlibHeader[2] = {0x78, 0x01};
    chunk_put(&o, kZlibHeader, 2);
  }
  for (y = 0; y < l->height; ++y) {
    const uint8_t* row = px + (size_t)y * l->stride;
    for (r = 0; r < rows; ++r) {
      uint8_t block[5];
      uint8_t const filter = 0;
      int const last = (y + 1 == l->height) && (r + 1 == rows);
      block[0] = (uint8_t)(last ? 1 : 0);
      block[1] = (uint8_t)(line & 0xFF);
      block[2] = (uint8_t)(line >> 8);
      block[3] = (uint8_t)(~line & 0xFF);
      block[4] = (uint8_t)((~line >> 8) & 0xFF);
      chunk_put(&o, block, 5);
      chunk_put(&o, &filter, 1);
      adler_update(&o, &filter, 1);
      chunk_put(&o, row, row_bytes);
      adler_update(&o, row, row_bytes);
    }
  }
  {
    uint8_t a[4];
    a[0] = (uint8_t)(o.adler >> 24);
    a[1] = (uint8_t)(o.adler >> 16);
    a[2] = (uint8_t)(o.adler >> 8);
    a[3] = (uint8_t)o.adler;
    chunk_put(&o, a, 4);
  }
  chunk_end(&o);

  chunk_begin(&o, 0, "IEND");
  chunk_end(&o);
  return o.ok;
}

static int write_ppm(FILE* f, const Layout* l, const uint8_t* px, int rows) {
  uint32_t y;
  int r;
  if (fprintf(f, "P6\n%u %u\n255\n", (unsigned)l->width,
              (unsigned)(l->height * (uint32_t)rows)) < 0)
    return 0;
  for (y = 0; y < l->height; ++y)
    for (r = 0; r < rows; ++r)
      if (fwrite(px + (size_t)y * l->stride, 1, (size_t)l->width * 3, f) !=
          (size_t)l->width * 3)
        return 0;
  return 1;
}

static int ends_with(const char* s, const char* suffix) {
  size_t const n = strlen(s), m = strlen(suffix);
  size_t i;
  if (n < m) return 0;
  for (i = 0; i < m; ++i) {
    char c = s[n - m + i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != suffix[i]) return 0;
  }
  return 1;
}

static int usage(void) {
  fprintf(stderr,
          "usage: fb_dump [-1] [-q] <ring name | core pid> <out.png|out.ppm>\n"
          "  copies the latest frame of a core's frame ring into a picture\n"
          "  (docs/protocol/fb-ring-v1.md)\n");
  return 1;
}

int main(int argc, char** argv) {
  const char* ring_arg = NULL;
  const char* out_path = NULL;
  char name[64];
  int rows = 2, quiet = 0, i, status, png, written;
  Ring ring;
  Layout layout;
  uint8_t* pixels;
  uint64_t seq = 0;
  unsigned waited;
  FILE* f;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-1") == 0)
      rows = 1;
    else if (strcmp(argv[i], "-q") == 0)
      quiet = 1;
    else if (argv[i][0] == '-' && argv[i][1] != '\0')
      return usage();
    else if (ring_arg == NULL)
      ring_arg = argv[i];
    else if (out_path == NULL)
      out_path = argv[i];
    else
      return usage();
  }
  if (ring_arg == NULL || out_path == NULL) return usage();
  png = ends_with(out_path, ".png");
  if (!png && !ends_with(out_path, ".ppm")) {
    fprintf(stderr, "fb_dump: %s: write a .png or a .ppm\n", out_path);
    return 1;
  }
  if (strspn(ring_arg, "0123456789") == strlen(ring_arg)) {
    /* A pid: the name the core with that pid publishes under. */
#ifdef _WIN32
    snprintf(name, sizeof(name), "Local\\kcpc-fb-%s", ring_arg);
#else
    snprintf(name, sizeof(name), "/kcpc-fb-%s", ring_arg);
#endif
  } else if (strlen(ring_arg) < sizeof(name)) {
    memcpy(name, ring_arg, strlen(ring_arg) + 1);
  } else {
    fprintf(stderr, "fb_dump: %s: not a ring name\n", ring_arg);
    return 1;
  }

  if (!ring_open(&ring, name)) return 2;
  if (!read_layout(&ring, &layout)) {
    fprintf(stderr, "fb_dump: %s is not a version %u RGB24 frame ring\n", name,
            (unsigned)kVersion);
    ring_close(&ring);
    return 2;
  }
  pixels = (uint8_t*)malloc((size_t)layout.slot_bytes);
  if (pixels == NULL) {
    fprintf(stderr, "fb_dump: out of memory\n");
    ring_close(&ring);
    return 2;
  }
  /* A core that has just started may not have finished its first frame:
   * wait for it, a second at most. */
  for (waited = 0;; waited += 10) {
    status = read_latest(&ring, &layout, pixels, &seq);
    if (status != 0 || waited >= 1000) break;
    sleep_ms(10);
  }
  ring_close(&ring);
  if (status != 1) {
    fprintf(stderr, "fb_dump: %s\n",
            status == 0 ? "no frame published yet"
                        : "the producer rewrote every slot during the copy");
    free(pixels);
    return 2;
  }

  crc_init();
  {
    /* The CRC32 of the frame's rows, top row first: what `hash frame`
     * reports when stride is width * 3, as it is in version 1. */
    uint32_t crc = 0, y;
    for (y = 0; y < layout.height; ++y)
      crc = crc_update(crc, pixels + (size_t)y * layout.stride,
                       (size_t)layout.width * 3);
    f = fopen(out_path, "wb");
    if (f == NULL) {
      perror("fb_dump: fopen");
      free(pixels);
      return 3;
    }
    written = png ? write_png(f, &layout, pixels, rows)
                  : write_ppm(f, &layout, pixels, rows);
    if (fclose(f) != 0) written = 0;
    free(pixels);
    if (!written) {
      fprintf(stderr, "fb_dump: cannot write %s\n", out_path);
      return 3;
    }
    if (!quiet)
      printf("seq=%llu crc32=%08X width=%u height=%u\n",
             (unsigned long long)seq, (unsigned)crc, (unsigned)layout.width,
             (unsigned)layout.height);
  }
  return 0;
}
