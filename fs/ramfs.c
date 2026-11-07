/*
 * SecOS Kernel - RAMFS Implementation
 * In-memory hierarchical filesystem supporting mutable & immutable entries.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 *
 * [M51 C2] Locking. One klock (LK_K30_FS, "ramfs") guards the whole instance:
 * the entry table here (names, flags, data pointers and sizes) and, in
 * ramfs_vfs.c, the VFS inode cache. Every exported function takes it; the
 * ramfs_*_locked() variants expect it held (KLOCK_ASSERT_HELD with lockdep)
 * and are what the VFS adapter calls inside its own lock section. Inside the
 * lock only the heap (S70) and the page-cache index (S40, from
 * pagecache_invalidate) are taken: never a filesystem, a user copy or a wait.
 * Entry slots never move (a removal leaves a free slot), so an entry pointer
 * stays a valid slot for the life of the kernel, but its contents are only
 * stable while the lock is held: a caller of the pointer-returning functions
 * (ramfs_find, ramfs_list, ramfs_list_path) that reads the entry afterwards
 * must hold ramfs_fs_lock() itself. (The kernel shell's rf* debug commands
 * still read them bare, under the BKL.)
 */
#include "ramfs.h"
#include <stdint.h>
#include <stddef.h>
#include "heap.h" // kmalloc/kfree

static klock_t g_ramfs_lock = KLOCK_INIT(LK_K30_FS, "ramfs");
klock_t* ramfs_fs_lock(void){ return &g_ramfs_lock; }
#define RAMFS_LOCKED() KLOCK_ASSERT_HELD(&g_ramfs_lock)

static ramfs_entry_t ramfs_table[RAMFS_MAX_FILES];
static size_t ramfs_count = 0;
static int ramfs_inited = 0;

static int str_eq(const char* a,const char* b){ while(*a && *b){ if(*a!=*b) return 0; a++; b++; } return *a==0 && *b==0; }
static size_t str_len(const char* s){ size_t l=0; while(s[l]) l++; return l; }
static void str_copy(char* dst,const char* src,size_t max){ size_t i=0; for(; i<max-1 && src[i]; i++) dst[i]=src[i]; dst[i]=0; }
static int str_starts(const char* s,const char* prefix){ size_t i=0; while(prefix[i]){ if(s[i]!=prefix[i]) return 0; i++; } return 1; }

static int ramfs_add_common(const char* name, const void* data, size_t size, unsigned flags){
    RAMFS_LOCKED();
    if(!name || !*name || str_len(name) >= RAMFS_NAME_MAX) return -1;
    for(size_t i=0;i<ramfs_count;i++) if(str_eq(ramfs_table[i].name, name)) return -1; // dup
    // Reuse a slot freed by ramfs_remove() before growing the table.
    ramfs_entry_t* e = NULL;
    for(size_t i=0;i<ramfs_count;i++) if(!ramfs_table[i].name[0]){ e = &ramfs_table[i]; break; }
    int appended = 0;
    if(!e){ if(ramfs_count >= RAMFS_MAX_FILES) return -1; e = &ramfs_table[ramfs_count++]; appended = 1; }
    uint8_t* buf = NULL;
    if(size && !(flags & 1)){                  // mutable file: a private heap copy
        buf = (uint8_t*)kmalloc(size);
        if(!buf){ if(appended) ramfs_count--; return -1; }
        for(size_t i=0;i<size;i++) buf[i] = ((const uint8_t*)data)[i];
    }
    e->flags = flags;
    if(size==0){ e->data = NULL; e->size = 0; }                                 // dir / empty file
    else if(flags & 1){ e->data = (uint8_t*)(uintptr_t)data; e->size = size; } // immutable: by reference
    else { e->data = buf; e->size = size; }
    str_copy(e->name, name, RAMFS_NAME_MAX);   // last: a named slot is a complete entry
    return 0;
}
int ramfs_add_locked(const char* name, const void* data, size_t size){ return ramfs_add_common(name,data,size,0); }

const ramfs_entry_t* ramfs_find_locked(const char* name){
    RAMFS_LOCKED();
    if(!name || !*name) return NULL;
    for(size_t i=0;i<ramfs_count;i++) if(str_eq(ramfs_table[i].name,name)) return &ramfs_table[i];
    return NULL;
}

// Write into one entry. The VFS adapter passes the entry its inode is bound
// to, so a rename running between the caller's lookup and the write cannot
// redirect the bytes by name.
int ramfs_write_entry_locked(ramfs_entry_t* e, size_t offset, const void* src, size_t len){
    RAMFS_LOCKED();
    if(!e || !e->name[0]) return -1;              // gone
    if(e->flags & 3) return -1;                   // immutable file / directory
    if(offset > e->size) return -1;               // disallow holes
    if(len > 0x7FFFFFFFu || offset + len < offset) return -1;
    size_t end = offset + len;
    if(end > e->size){ // need grow
        uint8_t* new_buf = (uint8_t*)kmalloc(end);
        if(!new_buf) return -1;
        for(size_t i=0;i<e->size;i++) new_buf[i]=e->data[i];
        if(e->data) kfree(e->data);
        e->data = new_buf;
        e->size = end;
    }
    for(size_t i=0;i<len;i++) e->data[offset+i] = ((const uint8_t*)src)[i];
    return (int)len;
}
int ramfs_write_locked(const char* name, size_t offset, const void* src, size_t len){
    return ramfs_write_entry_locked((ramfs_entry_t*)ramfs_find_locked(name), offset, src, len);
}

int ramfs_truncate_locked(const char* name, size_t new_size){
    return ramfs_truncate_entry_locked((ramfs_entry_t*)ramfs_find_locked(name), new_size);
}
// [M52 D3] Truncate the entry itself (an open file's inode points at it,
// whatever its name now). -1 for NULL, immutable or a directory.
int ramfs_truncate_entry_locked(ramfs_entry_t* e, size_t new_size){
    RAMFS_LOCKED();
    if(!e) return -1;
    if(e->flags & 3) return -1;                   // immutable file / directory
    if(new_size == e->size) return 0;
    if(new_size == 0){ // kmalloc(0) returns NULL: O_TRUNC used to fail and keep the old bytes
        if(e->data) kfree(e->data);
        e->data = NULL;
        e->size = 0;
        return 0;
    }
    uint8_t* new_buf = (uint8_t*)kmalloc(new_size);
    if(!new_buf) return -1;
    size_t copy = (new_size < e->size)? new_size : e->size;
    for(size_t i=0;i<copy;i++) new_buf[i]=e->data[i];
    for(size_t i=copy;i<new_size;i++) new_buf[i]=0;     // a grown file reads zeros (POSIX)
    if(e->data) kfree(e->data);
    e->data = new_buf;
    e->size = new_size;
    return 0;
}

int ramfs_remove_locked(const char* name){
    RAMFS_LOCKED();
    for(size_t i=0;i<ramfs_count;i++){
        if(str_eq(ramfs_table[i].name,name)){
            if(ramfs_table[i].flags & 1) return -1; // immutable entry not removable
            // free buffer
            if(ramfs_table[i].data) kfree(ramfs_table[i].data);
            // Leave a free slot instead of shifting the table: open VFS inodes keep
            // a pointer to their entry (fs_data), and shifting re-pointed every open
            // file behind the removed one at its neighbour (gzip's output landed in
            // another file after it unlinked its input).
            ramfs_table[i].name[0] = 0;
            ramfs_table[i].data = NULL;
            ramfs_table[i].size = 0;
            ramfs_table[i].flags = 0;
            if(i + 1 == ramfs_count) ramfs_count--;
            return 0;
        }
    }
    return -1; // not found
}

size_t ramfs_list_locked(const ramfs_entry_t** out_array, size_t max){
    RAMFS_LOCKED();
    size_t n=0;
    for(size_t i=0;i<ramfs_count && n<max;i++) if(ramfs_table[i].name[0]) out_array[n++]=&ramfs_table[i];
    return n;
}
static int is_root_path(const char* path){ return !path || path[0]==0 || (path[0]=='/' && path[1]==0); }
int ramfs_is_dir_locked(const char* path){
    if(is_root_path(path)) return 1; // treat root as directory
    const ramfs_entry_t* e=ramfs_find_locked(path); if(!e) return -1; return (e->flags & 2)?1:0;
}
// Is entry `name` a direct child of directory `path` (root: every top-level
// name, i.e. one without '/')?
static int is_child_of(const char* name, const char* path, int root){
    if(!name[0]) return 0;   // free slot
    if(root){ for(size_t k=0; name[k]; k++) if(name[k]=='/') return 0; return 1; }
    size_t plen=str_len(path);
    if(!str_starts(name,path) || name[plen]!='/') return 0;
    const char* sub = name+plen+1;
    for(size_t k=0; sub[k]; k++) if(sub[k]=='/') return 0;   // a deeper descendant
    return 1;
}
size_t ramfs_list_path_locked(const char* path, const ramfs_entry_t** out_array, size_t max){
    RAMFS_LOCKED();
    // List only direct children of path (root: path=="" or "/" -> all top-level entries lacking '/').
    int root = is_root_path(path);
    if(!root && ramfs_is_dir_locked(path)!=1) return 0;
    size_t count=0;
    for(size_t i=0;i<ramfs_count && count<max;i++)
        if(is_child_of(ramfs_table[i].name, path, root)) out_array[count++]=&ramfs_table[i];
    return count;
}
// [M51 C2] Cursor-based listing for a caller that drops the lock between
// entries (the VFS readdir runs its callback unlocked): the first direct child
// of `path` at table index >= *cursor, or NULL at the end; *cursor moves past
// it. Start with *cursor = 0. An entry created or removed between two calls
// may or may not be returned, as with readdir(3).
const ramfs_entry_t* ramfs_next_child_locked(const char* path, size_t* cursor){
    RAMFS_LOCKED();
    int root = is_root_path(path);
    if(*cursor == 0 && !root && ramfs_is_dir_locked(path)!=1){ *cursor = RAMFS_MAX_FILES; return NULL; }
    for(size_t i=*cursor;i<ramfs_count;i++){
        if(is_child_of(ramfs_table[i].name, path, root)){ *cursor = i + 1; return &ramfs_table[i]; }
    }
    *cursor = RAMFS_MAX_FILES;
    return NULL;
}

static int path_validate_parent(const char* path){ // verify each parent component exists and is a directory
    for(size_t i=0; path[i]; i++){
        if(path[i]=='/'){
            // parent = substring path[0..i-1]
            char parent[RAMFS_NAME_MAX]; if(i>=RAMFS_NAME_MAX) return 0; for(size_t k=0;k<i;k++) parent[k]=path[k]; parent[i]=0;
            const ramfs_entry_t* pe = ramfs_find_locked(parent); if(!pe || !(pe->flags & 2)) return 0; // parent must be directory
        }
    }
    return 1;
}
int ramfs_mkdir_locked(const char* path){
    RAMFS_LOCKED();
    if(!path || !*path) return -1;
    if(str_len(path)>=RAMFS_NAME_MAX) return -1;
    if(ramfs_find_locked(path)) return -1;
    if(!path_validate_parent(path)) return -1;
    return ramfs_add_common(path,"",0,2); // directory flag
}
int ramfs_rmdir_locked(const char* path){
    ramfs_entry_t* e = (ramfs_entry_t*)ramfs_find_locked(path); if(!e) return -1; if(!(e->flags & 2)) return -1; if(e->flags & 1) return -1; // immutable directory not removable
    // ensure empty: no entries whose name starts with path + '/'
    size_t plen=str_len(path);
    for(size_t i=0;i<ramfs_count;i++){
        if(ramfs_table[i].name!=e->name){
            if(str_starts(ramfs_table[i].name,path) && ramfs_table[i].name[plen]=='/') return -1; // has child
        }
    }
    // reuse remove
    return ramfs_remove_locked(path);
}

int ramfs_rename_locked(const char* old_path, const char* new_path){
    RAMFS_LOCKED();
    if(!old_path || !new_path || !*old_path || !*new_path) return -1;
    if(str_len(new_path) >= RAMFS_NAME_MAX) return -1;
    if(ramfs_find_locked(new_path)) return -1; // target already exists
    ramfs_entry_t* e = (ramfs_entry_t*)ramfs_find_locked(old_path); if(!e) return -1; if(e->flags & 1) return -1; // immutable cannot be renamed
    // Prevent cycles: renaming a directory into one of its descendants (/a -> /a/b) disallowed
    if((e->flags & 2) && str_starts(new_path, old_path)){
        size_t ol = str_len(old_path);
        if(new_path[ol]=='/' && new_path[ol+1]){
            // new_path has old_path as prefix followed by '/' => target is a descendant of source
            return -1;
        }
    }
    // Validate destination parent
    // Extract parent component of new_path if contains '/'
    const char* slash_last = NULL; for(size_t i=0; new_path[i]; i++){ if(new_path[i]=='/') slash_last=&new_path[i]; }
    if(slash_last){ size_t plen = (size_t)(slash_last - new_path); char parent[RAMFS_NAME_MAX]; for(size_t i=0;i<plen && i<RAMFS_NAME_MAX-1;i++) parent[i]=new_path[i]; parent[plen]=0; const ramfs_entry_t* pe = ramfs_find_locked(parent); if(!pe || !(pe->flags & 2)) return -1; }
    // If directory with children and prefix changes, update child paths accordingly
    int is_dir = (e->flags & 2)?1:0; char old_name[RAMFS_NAME_MAX]; str_copy(old_name,e->name,RAMFS_NAME_MAX);
    str_copy(e->name,new_path,RAMFS_NAME_MAX);
    if(is_dir){ size_t old_len = str_len(old_name); for(size_t i=0;i<ramfs_count;i++){ if(ramfs_table[i].name==e->name) continue; if(str_starts(ramfs_table[i].name, old_name) && ramfs_table[i].name[old_len]=='/'){
                // rebuild new child path: new_path + remainder
                const char* rest = ramfs_table[i].name + old_len; // includes leading '/'
                char new_child[RAMFS_NAME_MAX]; size_t cp=0; size_t j=0; while(new_path[j] && cp<RAMFS_NAME_MAX-1) new_child[cp++]=new_path[j++]; j=0; while(rest[j] && cp<RAMFS_NAME_MAX-1) new_child[cp++]=rest[j++]; new_child[cp]=0; str_copy(ramfs_table[i].name,new_child,RAMFS_NAME_MAX);
            }
        }
    }
    return 0;
}

/* ---- exported entry points: one lock section each ---- */
#define RAMFS_WRAP(type, call) \
    do { klock_lock(&g_ramfs_lock); type r_ = (call); klock_unlock(&g_ramfs_lock); return r_; } while (0)
int ramfs_add(const char* name, const void* data, size_t size){ RAMFS_WRAP(int, ramfs_add_common(name,data,size,0)); }
int ramfs_add_static(const char* name, const void* data, size_t size){ RAMFS_WRAP(int, ramfs_add_common(name,data,size,1)); }
int ramfs_write(const char* name, size_t offset, const void* src, size_t len){ RAMFS_WRAP(int, ramfs_write_locked(name,offset,src,len)); }
int ramfs_truncate(const char* name, size_t new_size){ RAMFS_WRAP(int, ramfs_truncate_locked(name,new_size)); }
int ramfs_remove(const char* name){ RAMFS_WRAP(int, ramfs_remove_locked(name)); }
const ramfs_entry_t* ramfs_find(const char* name){ RAMFS_WRAP(const ramfs_entry_t*, ramfs_find_locked(name)); }
size_t ramfs_list(const ramfs_entry_t** out_array, size_t max){ RAMFS_WRAP(size_t, ramfs_list_locked(out_array,max)); }
int ramfs_is_dir(const char* path){ RAMFS_WRAP(int, ramfs_is_dir_locked(path)); }
size_t ramfs_list_path(const char* path, const ramfs_entry_t** out_array, size_t max){ RAMFS_WRAP(size_t, ramfs_list_path_locked(path,out_array,max)); }
int ramfs_mkdir(const char* path){ RAMFS_WRAP(int, ramfs_mkdir_locked(path)); }
int ramfs_rmdir(const char* path){ RAMFS_WRAP(int, ramfs_rmdir_locked(path)); }
int ramfs_rename(const char* old_path, const char* new_path){ RAMFS_WRAP(int, ramfs_rename_locked(old_path,new_path)); }

static void ramfs_init_locked(void){ // sample immutable files & dirs
    static const char readme_txt[] = "SecOS RAMFS\nThis is a demonstrative in-memory filesystem.\n";
    static const char hello_txt[] = "Hello from RAMFS!\n";
    ramfs_add_common("README.txt", readme_txt, sizeof(readme_txt)-1, 1);
    ramfs_add_common("hello.txt", hello_txt, sizeof(hello_txt)-1, 1);
    // Example mutable directory
    ramfs_mkdir_locked("docs");
    ramfs_add_common("docs/info.txt","RAMFS documentation directory",29,0);
    // Version metadata
    static char version_buf[128];
    // Populate dynamic version (uses macros defined by Makefile)
#ifdef BUILD_TS
    const char* ts = BUILD_TS;
#else
    const char* ts = "UNKNOWN_TS";
#endif
#ifdef GIT_HASH
    const char* gh = GIT_HASH;
#else
    const char* gh = "NOHASH";
#endif
    const char* basev = "0.1.0-dev";
    size_t vp=0; // bounded write
    const char* parts[] = {"VERSION=", basev, "\nBUILD_TS=", ts, "\nGIT_HASH=", gh, "\n"};
    for(int pi=0; pi<7; pi++){ const char* s=parts[pi]; for(size_t k=0; s[k] && vp<sizeof(version_buf)-1; k++) version_buf[vp++]=s[k]; }
    version_buf[vp]=0;
    ramfs_add_common("VERSION", version_buf, vp, 1);
    // Syscalls listing (placeholder)
    static const char syscalls_txt[] = "0 exit\n1 write\n2 read\n3 sleep\n"; ramfs_mkdir_locked("sys"); ramfs_add_common("sys/syscalls.txt", syscalls_txt, sizeof(syscalls_txt)-1, 1);
    // Init script: each line a shell command
    static const char init_rc_txt[] = "# SecOS init.rc - shell commands here run at boot (quiet by default)\n"; ramfs_add_common("init.rc", init_rc_txt, sizeof(init_rc_txt)-1, 1);
    // Dynamic manifest: list initial entries (after base creation) under sys/manifest.txt
    {
        static char manifest_buf[4096]; size_t mp=0;
        const char* hdr = "# SecOS RAMFS Manifest\n# List of initial entries\n"; for(size_t i=0; hdr[i] && mp<sizeof(manifest_buf)-1; i++) manifest_buf[mp++]=hdr[i];
        static const ramfs_entry_t* arr[RAMFS_MAX_FILES]; size_t n = ramfs_list_locked(arr,RAMFS_MAX_FILES);
        for(size_t i=0;i<n;i++){
            const ramfs_entry_t* e=arr[i];
            const char* type = (e->flags & 2)?"DIR":"FILE";
            const char* name = e->name;
            // Line format: TYPE size name\n
            const char* sep1 = type; for(size_t k=0; sep1[k] && mp<sizeof(manifest_buf)-1; k++) manifest_buf[mp++]=sep1[k];
            manifest_buf[mp++]=' ';
            // size in dec
            char num[32]; size_t sz=e->size; int ni=0; if(sz==0){ num[ni++]='0'; } else { char tmp[32]; int ti=0; while(sz){ tmp[ti++]=(char)('0'+(sz%10)); sz/=10; } while(ti>0) num[ni++]=tmp[--ti]; }
            num[ni]=0; for(int k=0; num[k] && mp<sizeof(manifest_buf)-1; k++) manifest_buf[mp++]=num[k];
            manifest_buf[mp++]=' ';
            for(size_t k=0; name[k] && mp<sizeof(manifest_buf)-1; k++) manifest_buf[mp++]=name[k];
            manifest_buf[mp++]='\n';
            if(mp>=sizeof(manifest_buf)-2) break; // prevent overflow
        }
        manifest_buf[mp]=0;
        ramfs_add_common("sys/manifest.txt", manifest_buf, mp, 1);
    }
}
int ramfs_init(void){
    klock_lock(&g_ramfs_lock);
    if(!ramfs_inited){ ramfs_inited = 1; ramfs_init_locked(); }
    klock_unlock(&g_ramfs_lock);
    return 0;
}
