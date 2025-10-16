#include "fat32.h"
#include "vfs.h"
#include "ramfs.h" // reuse ramfs_add_common style via public APIs (temporary)
#include <stdint.h>

// For now we simulate by reading first sector and parsing BPB, then create placeholder files under /fat32.
// Future: implement real FAT directory parsing.

static int parse_bpb(block_dev_t* dev, fat32_bpb_t* out){
    uint8_t sector[1024]; // assume <= 512
    if(dev->sector_size > sizeof(sector)) return -1;
    if(dev->read(dev,0,sector,1)!=1) return -1;
    // Offsets per FAT32 spec (BPB)
    out->bytes_per_sector = *(uint16_t*)(sector+11);
    out->sectors_per_cluster = sector[13];
    out->reserved_sectors = *(uint16_t*)(sector+14);
    out->fat_count = sector[16];
    out->sectors_per_fat = *(uint32_t*)(sector+36);
    out->root_cluster = *(uint32_t*)(sector+44);
    out->total_sectors_32 = *(uint32_t*)(sector+32);
    // minimal sanity
    if(out->bytes_per_sector==0 || out->sectors_per_cluster==0 || out->fat_count==0) return -1;
    return 0;
}

// Temporary simple FS ops that expose only root listing of placeholder entries.
typedef struct fat32_inode { char path[256]; int is_dir; size_t size; } fat32_inode_t;
static vfs_inode_t* fat32_lookup(const char* path){ (void)path; return NULL; }
static int fat32_readdir(const char* dir, vfs_iter_cb cb, void* user){ (void)dir; (void)cb; (void)user; return 0; }
static int fat32_read(vfs_inode_t* i,size_t o,void* b,size_t l){ (void)i;(void)o;(void)b;(void)l; return -1; }
static int fat32_write(vfs_inode_t* i,size_t o,const void* d,size_t l){ (void)i;(void)o;(void)d;(void)l; return -1; }
static int fat32_create(const char* p,const void* d,size_t s){ (void)p;(void)d;(void)s; return -1; }
static int fat32_mkdir(const char* p){ (void)p; return -1; }
static int fat32_remove(const char* p){ (void)p; return -1; }
static int fat32_rename(const char* a,const char* b){ (void)a;(void)b; return -1; }
static int fat32_truncate(const char* p,size_t n){ (void)p;(void)n; return -1; }

static vfs_fs_ops_t fat32_ops = {
    .lookup=fat32_lookup,
    .readdir=fat32_readdir,
    .read=fat32_read,
    .write=fat32_write,
    .create=fat32_create,
    .mkdir=fat32_mkdir,
    .remove=fat32_remove,
    .rename=fat32_rename,
    .truncate=fat32_truncate
};

static void append_dec_field(char* buf,int* p,int cap,uint32_t v,const char* label){
    for(int i=0;label[i] && *p<cap-1;i++) buf[(*p)++]=label[i];
    char tmp[16]; int ti=0; if(v==0){ tmp[ti++]='0'; } else { uint32_t x=v; char rev[16]; int ri=0; while(x && ri<16){ rev[ri++]=(char)('0'+(x%10)); x/=10; } while(ri>0) tmp[ti++]=rev[--ri]; }
    for(int i=0; i<ti && *p<cap-1; i++) buf[(*p)++]=tmp[i];
    if(*p<cap-1) buf[(*p)++]='\n';
}
int fat32_mount(const char* dev_name, const char* mount_point){ (void)mount_point; block_dev_t* dev = block_find(dev_name); if(!dev) return -1; fat32_bpb_t bpb; if(parse_bpb(dev,&bpb)!=0) return -1; char info[128]; int p=0; const char* hdr="FAT32 BPB\n"; for(int i=0;hdr[i]&&p<120;i++) info[p++]=hdr[i];
    append_dec_field(info,&p,128,bpb.bytes_per_sector,"bytes_per_sector=");
    append_dec_field(info,&p,128,bpb.sectors_per_cluster,"sectors_per_cluster=");
    append_dec_field(info,&p,128,bpb.reserved_sectors,"reserved_sectors=");
    append_dec_field(info,&p,128,bpb.fat_count,"fat_count=");
    append_dec_field(info,&p,128,bpb.sectors_per_fat,"sectors_per_fat=");
    append_dec_field(info,&p,128,bpb.root_cluster,"root_cluster=");
    append_dec_field(info,&p,128,bpb.total_sectors_32,"total_sectors=");
    if(p>=127) p=127; info[p]=0; ramfs_add("fat32_bpb.txt", info, p); return 0; }
