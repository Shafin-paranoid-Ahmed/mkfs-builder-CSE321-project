#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <stdlib.h>
#include <sys/stat.h>

static size_t cstrnlen(const char *s, size_t max){
    size_t i = 0;
    while (i < max && s[i] != '\0') ++i;
    return i;
}




#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
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
    
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;          
    uint8_t  type;              
    char     name[58];            

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

// ---------- bitmap + block helpers ----------
static inline uint8_t* blk_ptr(uint8_t *img, uint64_t blk_no){
    return img + (size_t)blk_no * BS;
}
static inline void bm_set(uint8_t *bm, uint32_t i){ bm[i>>3] |= (uint8_t)(1u<<(i&7)); }
static inline int  bm_get(const uint8_t *bm, uint32_t i){ return (bm[i>>3] >> (i&7)) & 1u; }
static int bm_find_zero(const uint8_t *bm, uint32_t limit){
    for (uint32_t i=0;i<limit;i++) if (!bm_get(bm,i)) return (int)i;
    return -1;
}

// ---------- CLI ----------
typedef struct {
    char input[512];
    char output[512];
    char filepath[1024];
} cli_t;

static void usage(const char* prog){
    fprintf(stderr,
      "Usage:\n"
      "  %s --input in.img --output out.img --file host_path\n", prog);
}
static int parse_cli(int argc, char **argv, cli_t *cli){
    memset(cli,0,sizeof(*cli));
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--input") && i+1<argc) {
            strncpy(cli->input, argv[++i], sizeof(cli->input)-1);
        } else if (!strcmp(argv[i],"--output") && i+1<argc) {
            strncpy(cli->output, argv[++i], sizeof(cli->output)-1);
        } else if (!strcmp(argv[i],"--file") && i+1<argc) {
            strncpy(cli->filepath, argv[++i], sizeof(cli->filepath)-1);
        } else {
            usage(argv[0]); return -1;
        }
    }
    if (cli->input[0]=='\0' || cli->output[0]=='\0' || cli->filepath[0]=='\0'){
        usage(argv[0]); return -1;
    }
    if (!strcmp(cli->input, cli->output)){
        fprintf(stderr,"--output must be a NEW file (not same as --input)\n");
        return -1;
    }
    return 0;
}

// ---------- core: add a file to "/" ----------
static int add_file_to_root(const cli_t *cli){
    (void)&bm_find_zero;
    // read input image
    FILE *fp = fopen(cli->input, "rb");
    if (!fp){ perror("fopen input"); return -1; }
    if (fseek(fp, 0, SEEK_END) != 0){ perror("fseek"); fclose(fp); return -1; }
    long img_bytes = ftell(fp);
    if (img_bytes < 0){ perror("ftell"); fclose(fp); return -1; }
    rewind(fp);
    uint8_t *img = (uint8_t*)malloc((size_t)img_bytes);
    if (!img){ perror("malloc"); fclose(fp); return -1; }
    if (fread(img,1,(size_t)img_bytes,fp)!=(size_t)img_bytes){ perror("fread"); free(img); fclose(fp); return -1; }
    fclose(fp);

    // map core structures
    superblock_t *sb = (superblock_t*)blk_ptr(img, 0);
    if (sb->magic_number != 0x4D565346u || sb->version != 1 || sb->block_size != BS){
        fprintf(stderr,"invalid superblock (magic/version/block_size)\n");
        free(img); return -1;
    }
    if ((uint64_t)img_bytes != sb->total_blocks * BS){
        fprintf(stderr,"image size (%ld) != total_blocks*BS (%" PRIu64 ")\n",
                img_bytes, sb->total_blocks * (uint64_t)BS);
        free(img); return -1;
    }
    uint8_t *inode_bm   = blk_ptr(img, sb->inode_bitmap_start);
    uint8_t *data_bm    = blk_ptr(img, sb->data_bitmap_start);
    uint8_t *inode_base = blk_ptr(img, sb->inode_table_start);
    uint8_t *root_dir_block = blk_ptr(img, sb->data_region_start);

    // read host file
    struct stat st;
    if (stat(cli->filepath, &st) != 0){ perror("stat host file"); free(img); return -1; }
    uint64_t fsize = (uint64_t)st.st_size;
    if (fsize == 0){ fprintf(stderr,"refusing to add empty file\n"); free(img); return -1; }
    uint32_t need_blocks = (uint32_t)((fsize + BS - 1) / BS);
    if (need_blocks > DIRECT_MAX){
        fprintf(stderr,"file too large for 12 direct blocks (%u needed)\n", need_blocks);
        free(img); return -1;
    }

    // name (<=58 bytes)
    const char *slash = strrchr(cli->filepath, '/');
    const char *fname = slash ? slash+1 : cli->filepath;
    if (!fname || fname[0]=='\0'){ fprintf(stderr,"bad filename\n"); free(img); return -1; }
    char name58[59]; memset(name58,0,sizeof(name58));
    size_t n = cstrnlen(fname, 58);
    memcpy(name58, fname, n);
    if (n < 58){ 
        name58[n] = '\0'; 
    }


    size_t slots = BS / sizeof(dirent64_t);
    for (size_t i = 0; i < slots; i++) {    
        dirent64_t *de = (dirent64_t*)(root_dir_block + i*sizeof(dirent64_t));
        if (de->inode_no != 0 && de->type == 1 && strncmp(de->name, name58, 58)==0){
            fprintf(stderr,"a file named \"%s\" already exists in /\n", name58);
            free(img); return -1;
        }
    }

    // find free inode (skip root at index 0)
    int free_i_rel = -1;
    for (uint32_t i=1;i<sb->inodes_count;i++){ if (!bm_get(inode_bm,i)){ free_i_rel=(int)i; break; } }
    if (free_i_rel < 0){ fprintf(stderr,"no free inodes\n"); free(img); return -1; }
    uint32_t new_ino = (uint32_t)free_i_rel + 1; // inodes are 1-indexed

    // find free data blocks
    uint32_t picked = 0;
    uint32_t rel_blocks[DIRECT_MAX] = {0};
    for (uint32_t i=0;i<sb->data_region_blocks && picked<need_blocks;i++){
        if (!bm_get(data_bm, i)) rel_blocks[picked++] = i;
    }
    if (picked < need_blocks){ fprintf(stderr,"not enough free data blocks\n"); free(img); return -1; }

    // mark allocations in bitmaps
    bm_set(inode_bm, free_i_rel);
    for (uint32_t k=0;k<need_blocks;k++) bm_set(data_bm, rel_blocks[k]);

    // write file data
    FILE *hf = fopen(cli->filepath, "rb");
    if (!hf){ perror("fopen host"); free(img); return -1; }
    uint32_t abs_blocks[DIRECT_MAX]={0};
    for (uint32_t k=0;k<need_blocks;k++){
        uint64_t abs_blk = sb->data_region_start + rel_blocks[k];
        abs_blocks[k] = (uint32_t)abs_blk;
        uint8_t *dst = blk_ptr(img, abs_blk);
        size_t want = (k==need_blocks-1) ? (size_t)(fsize - (uint64_t)k*BS) : (size_t)BS;
        size_t rd = fread(dst,1,want,hf);
        if (rd != want){ perror("fread host"); fclose(hf); free(img); return -1; }
        if (want < BS) memset(dst+want, 0, BS-want);
    }
    fclose(hf);

    // create file inode
    uint64_t now = (uint64_t)time(NULL);
    inode_t node = (inode_t){0};
    node.mode       = 0100000;        // regular file
    node.links      = 1;
    node.uid        = 0;
    node.gid        = 0;
    node.size_bytes = fsize;
    node.atime = node.mtime = node.ctime = now;
    for (uint32_t k=0;k<need_blocks;k++) node.direct[k] = abs_blocks[k];
    inode_crc_finalize(&node);
    memcpy(inode_base + (size_t)(free_i_rel) * INODE_SIZE, &node, sizeof(node));

    // add dirent to root
    int free_slot = -1;
    size_t slots2 = BS / sizeof(dirent64_t);
    for (size_t i = 0; i < slots2; i++) {
        dirent64_t *de = (dirent64_t*)(root_dir_block + i*sizeof(dirent64_t));
        if (de->inode_no == 0){ 
            free_slot = i; 
            break; 
        }
    }
    if (free_slot < 0){ 
        fprintf(stderr,"root directory block is full\n"); 
        free(img); 
        return -1; 
    }

    dirent64_t newde = {0};
    newde.inode_no = new_ino;
    newde.type     = 1; // file
    size_t m = cstrnlen(name58, 58);
    memcpy(newde.name, name58, m);
    if (m < 58){ 
        newde.name[m] = '\0'; 
    }
    dirent_checksum_finalize(&newde);
    memcpy(root_dir_block + free_slot*sizeof(dirent64_t), &newde, sizeof(newde));

    // bump root inode link count + mtime
    inode_t *root = (inode_t*)(inode_base + 0 * INODE_SIZE);
    root->links += 1;                         
    root->size_bytes += sizeof(dirent64_t);   
    root->mtime = now;
    inode_crc_finalize(root);

    // update superblock time + checksum
    sb->mtime_epoch = now;
    superblock_crc_finalize(sb);

    // write to new output file
    FILE *out = fopen(cli->output, "wb");
    if (!out){ perror("fopen output"); free(img); return -1; }
    if (fwrite(img,1,(size_t)img_bytes,out)!=(size_t)img_bytes){ perror("fwrite output"); fclose(out); free(img); return -1; }
    fclose(out);
    free(img);

    printf("Added \"%s\" (%" PRIu64 " bytes) as inode #%u using %u block(s).\n",
           name58, fsize, new_ino, need_blocks);
    printf("Wrote new image: %s\n", cli->output);
    return 0;
}






int main(int argc, char **argv) {
    crc32_init();
    cli_t cli;
    if (parse_cli(argc, argv, &cli) != 0){
        return 2;
    }
    if (add_file_to_root(&cli) != 0){
        return 3;
    }
    return 0;
}