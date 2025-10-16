#include "ext2.h"
#include "ramfs.h" // per creare file diagnostico finché non abbiamo multi-mount
#include <stdint.h>

static int read_super(block_dev_t* dev, ext2_superblock_t* sb){
    // Superblock at offset 1024 bytes (sector-aligned typical). Assume 512 bytes per sector for demo.
    uint8_t buf[2048];
    // Read first 4 sectors (0..3) to cover superblock area at 1024 and following
    if(dev->read(dev,0,buf,4)!=4) return -1;
    // superblock start 1024 offset
    uint8_t* s = buf + 1024;
    sb->inodes_count = *(uint32_t*)(s+0);
    sb->blocks_count = *(uint32_t*)(s+4);
    sb->reserved_blocks_count = *(uint32_t*)(s+8);
    sb->free_blocks_count = *(uint32_t*)(s+12);
    sb->free_inodes_count = *(uint32_t*)(s+16);
    sb->first_data_block = *(uint32_t*)(s+20);
    sb->log_block_size = *(uint32_t*)(s+24);
    sb->log_frag_size = *(uint32_t*)(s+28);
    sb->blocks_per_group = *(uint32_t*)(s+32);
    sb->frags_per_group = *(uint32_t*)(s+36);
    sb->inodes_per_group = *(uint32_t*)(s+40);
    sb->mtime = *(uint32_t*)(s+44);
    sb->wtime = *(uint32_t*)(s+48);
    sb->mount_count = *(uint16_t*)(s+52);
    sb->max_mount_count = *(uint16_t*)(s+54);
    sb->magic = *(uint16_t*)(s+56);
    if(sb->magic != 0xEF53) return -1;
    return 0;
}

// Stub VFS ops (not actually mounted yet because single-root limitation)
static vfs_inode_t* ext2_lookup(const char* path){ (void)path; return NULL; }
static int ext2_readdir(const char* path, vfs_iter_cb cb, void* user){ (void)path;(void)cb;(void)user; return 0; }
static int ext2_read(vfs_inode_t* i,size_t o,void* b,size_t l){ (void)i;(void)o;(void)b;(void)l; return -1; }
static int ext2_write(vfs_inode_t* i,size_t o,const void* d,size_t l){ (void)i;(void)o;(void)d;(void)l; return -1; }
static int ext2_create(const char* p,const void* d,size_t s){ (void)p;(void)d;(void)s; return -1; }
static int ext2_mkdir(const char* p){ (void)p; return -1; }
static int ext2_remove(const char* p){ (void)p; return -1; }
static int ext2_rename(const char* a,const char* b){ (void)a;(void)b; return -1; }
static int ext2_truncate(const char* p,size_t n){ (void)p;(void)n; return -1; }

static vfs_fs_ops_t ext2_ops = {
    .lookup=ext2_lookup,
    .readdir=ext2_readdir,
    .read=ext2_read,
    .write=ext2_write,
    .create=ext2_create,
    .mkdir=ext2_mkdir,
    .remove=ext2_remove,
    .rename=ext2_rename,
    .truncate=ext2_truncate
};

int ext2_mount(const char* dev_name){ block_dev_t* dev = block_find(dev_name); if(!dev) return -1; ext2_superblock_t sb; if(read_super(dev,&sb)!=0) return -1; // produce diagnostic file
    char info[192]; int p=0; const char* hdr="EXT2 SUPERBLOCK\n"; for(int i=0;hdr[i]&&p<sizeof(info)-1;i++) info[p++]=hdr[i];
    // helper
    #define APP(label,val) do { const char* L=label; for(int i=0;L[i]&&p<sizeof(info)-1;i++) info[p++]=L[i]; uint32_t v=(uint32_t)(val); char tmp[16]; int ti=0; if(v==0){ tmp[ti++]='0'; } else { uint32_t x=v; char r[16]; int ri=0; while(x&&ri<16){ r[ri++]=(char)('0'+(x%10)); x/=10; } while(ri) tmp[ti++]=r[--ri]; } tmp[ti]=0; for(int i=0;tmp[i]&&p<sizeof(info)-1;i++) info[p++]=tmp[i]; if(p<sizeof(info)-1) info[p++]='\n'; } while(0)
    APP("inodes_count=", sb.inodes_count);
    APP("blocks_count=", sb.blocks_count);
    APP("free_blocks=", sb.free_blocks_count);
    APP("free_inodes=", sb.free_inodes_count);
    APP("block_size_log=", sb.log_block_size);
    APP("magic=", sb.magic);
    info[p]=0;
    ramfs_add("ext2_superblock.txt", info, p);
    // Replace root FS with ext2 (stub) for now
    vfs_replace_root(&ext2_ops, "ext2");
    return 0; }