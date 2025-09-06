// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>


#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

// bitmap helpers (1 = used, 0 = free)
static inline void bm_set(uint8_t *bm, uint32_t i){
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

// pointer to a block inside the image buffer
static inline uint8_t* blk_ptr(uint8_t *img, uint64_t blk_no){
    return img + (size_t)blk_no * BS;
}





#pragma pack(push, 1)
typedef struct {
    uint32_t magic_number;
    uint32_t version;
    uint32_t block_size;
    
    uint64_t total_blocks;
    uint64_t inodes_count;

    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;

    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;

    uint64_t inode_table_start;
    uint64_t inode_table_blocks;

    uint64_t data_region_start;
    uint64_t data_region_blocks;

    uint64_t root_inode;
    uint64_t mtime_epoch;

    uint32_t flags;

    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;

    uint32_t uid;
    uint32_t gid;

    uint64_t size_bytes;

    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;


    uint32_t direct[12];

    uint32_t reserved_0;          
    uint32_t reserved_1;          
    uint32_t reserved_2;
    
    uint32_t proj_id;
    uint32_t uid16_gid16;

    uint64_t xattr_ptr;
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];

    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}



typedef struct {
    char image[512];
    long size_kib;    
    long inodes;      
    unsigned long seed; 
} cli_t;

static void usage(const char *prog){
    fprintf(stderr,
      "Usage:\n"
      "  %s --image out.img --size-kib <180..4096> --inodes <128..512> [--seed N]\n",
      prog);
}

static int parse_long(const char *s, long *out){
    char *end = NULL; errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end==s || *end!='\0') return -1;
    *out = v; return 0;
}

static int parse_cli(int argc, char **argv, cli_t *cli){
    memset(cli, 0, sizeof(*cli));
    cli->seed = (unsigned long)time(NULL); // default

    for (int i=1; i<argc; i++){
        if (!strcmp(argv[i], "--image") && i+1<argc){
            strncpy(cli->image, argv[++i], sizeof(cli->image)-1);
        } else if (!strcmp(argv[i], "--size-kib") && i+1<argc){
            if (parse_long(argv[++i], &cli->size_kib)) { fprintf(stderr,"Invalid --size-kib\n"); return -1; }
        } else if (!strcmp(argv[i], "--inodes") && i+1<argc){
            if (parse_long(argv[++i], &cli->inodes)) { fprintf(stderr,"Invalid --inodes\n"); return -1; }
        } else if (!strcmp(argv[i], "--seed") && i+1<argc){
            long tmp; if (parse_long(argv[++i], &tmp)) { fprintf(stderr,"Invalid --seed\n"); return -1; }
            cli->seed = (unsigned long)tmp;
        } else {
            usage(argv[0]); return -1;
        }
    }

    if (cli->image[0]=='\0' || cli->size_kib==0 || cli->inodes==0) { usage(argv[0]); return -1; }
    if (cli->size_kib < 180 || cli->size_kib > 4096 || (cli->size_kib % 4)!=0){
        fprintf(stderr,"--size-kib must be 180..4096 and a multiple of 4\n"); return -1;
    }
    if (cli->inodes < 128 || cli->inodes > 512){
        fprintf(stderr,"--inodes must be 128..512\n"); return -1;
    }
    return 0;
}

static int layout_and_print(const cli_t *cli){
    const long total_blocks = cli->size_kib / 4;                     // (KiB / 4) because BS=4096
    const long inodes_per_blk = BS / INODE_SIZE;                      // 4096 / 128 = 32
    const long inode_table_blocks = (cli->inodes + inodes_per_blk - 1) / inodes_per_blk; // ceil

    const long superblock_block   = 0;
    const long inode_bitmap_block = 1;
    const long data_bitmap_block  = 2;
    const long inode_table_start  = 3;
    const long data_region_start  = inode_table_start + inode_table_blocks;
    const long data_region_blocks = total_blocks - data_region_start;

    if (data_region_blocks <= 0){
        fprintf(stderr,"Image too small: no room for data blocks.\n");
        return -1;
    }

    printf("OK: image=\"%s\", size_kib=%ld, inodes=%ld, seed=%lu\n",
           cli->image, cli->size_kib, cli->inodes, cli->seed);
    printf("Derived layout (blocks are 0-based):\n");
    printf("  BLOCK_SIZE           : %u bytes\n", (unsigned)BS);
    printf("  total_blocks         : %ld\n", total_blocks);
    printf("  inodes per block     : %ld\n", inodes_per_blk);
    printf("  inode_table_blocks   : %ld\n", inode_table_blocks);
    printf("  superblock           : %ld\n", superblock_block);
    printf("  inode_bitmap_block   : %ld\n", inode_bitmap_block);
    printf("  data_bitmap_block    : %ld\n", data_bitmap_block);
    printf("  inode_table_start    : %ld .. %ld\n",
           inode_table_start, inode_table_start + inode_table_blocks - 1);
    printf("  data_region_start    : %ld\n", data_region_start);
    printf("  data_region_blocks   : %ld\n", data_region_blocks);

    
    g_random_seed = cli->seed;
    return 0;
}

// Build the image in memory and write it to cli->image
static int build_and_save_fs(const cli_t *cli){
    // ---- recompute layout (same math you printed) ----
    const long total_blocks       = cli->size_kib / 4;                  // 4096B blocks
    const long inodes_per_blk     = BS / INODE_SIZE;                    // 32
    const long inode_table_blocks = (cli->inodes + inodes_per_blk - 1) / inodes_per_blk;

    const long superblock_block   = 0;
    const long inode_bitmap_block = 1;
    const long data_bitmap_block  = 2;
    const long inode_table_start  = 3;
    const long data_region_start  = inode_table_start + inode_table_blocks;
    const long data_region_blocks = total_blocks - data_region_start;

    if (data_region_blocks <= 0){
        fprintf(stderr, "Image too small: no room for data blocks.\n");
        return -1;
    }

    // ---- allocate zero-filled image ----
    const size_t img_bytes = (size_t)total_blocks * BS;
    uint8_t *img = (uint8_t*)calloc(1, img_bytes);
    if (!img){ perror("calloc"); return -1; }

    // ---- SUPERBLOCK ----
    superblock_t sb = {0};
    sb.magic_number        = 0x4D565346u;          // "MVFS"
    sb.version             = 1;
    sb.block_size          = BS;

    sb.total_blocks        = (uint64_t)total_blocks;
    sb.inodes_count        = (uint64_t)cli->inodes;

    sb.inode_bitmap_start  = (uint64_t)inode_bitmap_block;
    sb.inode_bitmap_blocks = 1;

    sb.data_bitmap_start   = (uint64_t)data_bitmap_block;
    sb.data_bitmap_blocks  = 1;

    sb.inode_table_start   = (uint64_t)inode_table_start;
    sb.inode_table_blocks  = (uint64_t)inode_table_blocks;

    sb.data_region_start   = (uint64_t)data_region_start;
    sb.data_region_blocks  = (uint64_t)data_region_blocks;

    sb.root_inode          = ROOT_INO;            // 1
    sb.mtime_epoch         = (uint64_t)time(NULL);
    sb.flags               = 0;

    (void)superblock_crc_finalize; // silence warning until we call it
    superblock_crc_finalize(&sb);  // compute & set sb.checksum

    memcpy(blk_ptr(img, superblock_block), &sb, sizeof(sb));

    // ---- BITMAPS ----
    uint8_t *ibm = blk_ptr(img, inode_bitmap_block); // inode bitmap block
    uint8_t *dbm = blk_ptr(img, data_bitmap_block);  // data bitmap block

    // mark inode #1 (root) as allocated (note: inodes are 1-indexed → bit 0)
    bm_set(ibm, ROOT_INO - 1);

    // mark first data block in data region as allocated (root dir's data block)
    bm_set(dbm, 0); // relative index 0 == absolute block 'data_region_start'

    // ---- ROOT INODE (#1) ----
    inode_t root = (inode_t){0};
    root.mode        = 0040000;                        // directory
    root.links       = 2;                              // "." and ".."
    root.uid         = 0;
    root.gid         = 0;
    root.size_bytes  = 2 * sizeof(dirent64_t);         // two entries
    uint64_t now     = (uint64_t)time(NULL);
    root.atime = root.mtime = root.ctime = now;
    root.proj_id   = 3;
    root.direct[0]   = (uint32_t)data_region_start;    // ABSOLUTE block number
    // other direct[] entries are already 0

    // finalize inode CRC AFTER all fields set
    inode_crc_finalize(&root);

    // write root inode into inode table slot 0 (inode #1)
    uint8_t *inode_table_base = blk_ptr(img, inode_table_start);
    memcpy(inode_table_base + 0 * INODE_SIZE, &root, sizeof(root));

    // ---- ROOT DIRECTORY DATA BLOCK ('.' and '..') ----
    dirent64_t de_dot  = {0};
    de_dot.inode_no    = ROOT_INO;
    de_dot.type        = 2;          // directory
    strcpy(de_dot.name, ".");
    dirent_checksum_finalize(&de_dot);

    dirent64_t de_ddot = {0};
    de_ddot.inode_no   = ROOT_INO;
    de_ddot.type       = 2;          // directory
    strcpy(de_ddot.name, "..");
    dirent_checksum_finalize(&de_ddot);

    uint8_t *root_dir_block = blk_ptr(img, data_region_start);
    memcpy(root_dir_block + 0,                     &de_dot,  sizeof(de_dot));
    memcpy(root_dir_block + sizeof(dirent64_t),    &de_ddot, sizeof(de_ddot));
    // rest of the block remains zero (free dirent slots)

    // ---- WRITE THE IMAGE TO DISK ----
    FILE *fp = fopen(cli->image, "wb");
    if (!fp){ perror("fopen"); free(img); return -1; }
    size_t wr = fwrite(img, 1, img_bytes, fp);
    if (wr != img_bytes){ perror("fwrite"); fclose(fp); free(img); return -1; }
    fclose(fp);
    free(img);

    printf("Wrote filesystem image: %s (%zu bytes)\n", cli->image, img_bytes);
    return 0;
}




int main(int argc, char **argv) {
    crc32_init();
    cli_t cli;
    if (parse_cli(argc, argv, &cli) != 0) {
        return 2;
    }
    if (layout_and_print(&cli) != 0) {
        return 3;
    }
    if (build_and_save_fs(&cli) != 0) {
        return 4;
    }
    return 0;
}
