/*
 * SecOS Kernel - RAMFS Implementation
 * In-memory hierarchical filesystem supporting mutable & immutable entries.
 * Copyright (c) 2025 iDev srl
 * Author: Luigi De Astis <l.deastis@idev-srl.com>
 * SPDX-License-Identifier: MIT
 */
#include "ramfs.h"
#include <stdint.h>
#include <stddef.h>
#include "heap.h" // kmalloc/kfree

static ramfs_entry_t ramfs_table[RAMFS_MAX_FILES];
static size_t ramfs_count = 0;
static int ramfs_inited = 0;

static int str_eq(const char* a,const char* b){ while(*a && *b){ if(*a!=*b) return 0; a++; b++; } return *a==0 && *b==0; }
static size_t str_len(const char* s){ size_t l=0; while(s[l]) l++; return l; }
static void str_copy(char* dst,const char* src,size_t max){ size_t i=0; for(; i<max-1 && src[i]; i++) dst[i]=src[i]; dst[i]=0; }
static int str_starts(const char* s,const char* prefix){ size_t i=0; while(prefix[i]){ if(s[i]!=prefix[i]) return 0; i++; } return 1; }
static const char* path_after(const char* full,const char* parent){ // assume parent ends without trailing '/'
    size_t i=0; while(parent[i]){ if(full[i]!=parent[i]) return NULL; i++; }
    if(full[i]=='/' && full[i+1]) return full+i+1; if(full[i]==0) return NULL; return NULL;
}

static int ramfs_add_common(const char* name, const void* data, size_t size, unsigned flags){
    if(ramfs_count >= RAMFS_MAX_FILES) return -1;
    if(!name || !*name || str_len(name) >= RAMFS_NAME_MAX) return -1;
    for(size_t i=0;i<ramfs_count;i++) if(str_eq(ramfs_table[i].name, name)) return -1; // dup
    ramfs_entry_t* e = &ramfs_table[ramfs_count++];
    str_copy(e->name, name, RAMFS_NAME_MAX);
    e->flags = flags;
    if(size==0){ // avoid kmalloc(0) for directories or initial empty file
        e->data = NULL;
        e->size = 0;
        return 0;
    }
    if(flags & 1){ // immutable: points directly to provided data
        e->data = (uint8_t*)(uintptr_t)data;
        e->size = size;
    } else {
    // copy into heap (mutable file)
        e->data = (uint8_t*)kmalloc(size);
        if(!e->data){ ramfs_count--; return -1; }
        for(size_t i=0;i<size;i++) e->data[i] = ((const uint8_t*)data)[i];
        e->size = size;
    }
    return 0;
}
int ramfs_add(const char* name, const void* data, size_t size){ return ramfs_add_common(name,data,size,0); }
int ramfs_add_static(const char* name, const void* data, size_t size){ return ramfs_add_common(name,data,size,1); }

int ramfs_write(const char* name, size_t offset, const void* src, size_t len){
    ramfs_entry_t* e = (ramfs_entry_t*)ramfs_find(name); if(!e) return -1; if(e->flags & 1) return -1; // immutable file not writable
    if(offset > e->size) return -1; // disallow holes
    size_t end = offset + len; if(end > e->size){ // need grow
        size_t new_size = end;
        uint8_t* new_buf = (uint8_t*)kmalloc(new_size);
        if(!new_buf) return -1;
        // copy old
        for(size_t i=0;i<e->size;i++) new_buf[i]=e->data[i];
        // free old
        kfree(e->data);
        e->data = new_buf;
        e->size = new_size;
    }
    // perform write
    for(size_t i=0;i<len;i++) e->data[offset+i] = ((const uint8_t*)src)[i];
    return (int)len;
}

int ramfs_truncate(const char* name, size_t new_size){
    ramfs_entry_t* e = (ramfs_entry_t*)ramfs_find(name); if(!e) return -1; if(e->flags & 1) return -1; // immutable file not truncatable
    if(new_size == e->size) return 0;
    uint8_t* new_buf = (uint8_t*)kmalloc(new_size);
    if(!new_buf) return -1;
    size_t copy = (new_size < e->size)? new_size : e->size;
    for(size_t i=0;i<copy;i++) new_buf[i]=e->data[i];
    kfree(e->data);
    e->data = new_buf;
    e->size = new_size;
    return 0;
}

int ramfs_remove(const char* name){
    for(size_t i=0;i<ramfs_count;i++){
        if(str_eq(ramfs_table[i].name,name)){
            if(ramfs_table[i].flags & 1) return -1; // immutable entry not removable
            // free buffer
            if(ramfs_table[i].data) kfree(ramfs_table[i].data);
            // shift remaining
            for(size_t j=i+1;j<ramfs_count;j++) ramfs_table[j-1]=ramfs_table[j];
            ramfs_count--;
            return 0;
        }
    }
    return -1; // not found
}

const ramfs_entry_t* ramfs_find(const char* name){ if(!name) return NULL; for(size_t i=0;i<ramfs_count;i++) if(str_eq(ramfs_table[i].name,name)) return &ramfs_table[i]; return NULL; }
size_t ramfs_list(const ramfs_entry_t** out_array, size_t max){ size_t n = (ramfs_count < max)? ramfs_count : max; for(size_t i=0;i<n;i++) out_array[i]=&ramfs_table[i]; return n; }
int ramfs_is_dir(const char* path){
    if(!path || path[0]==0 || (path[0]=='/' && path[1]==0)) return 1; // treat root as directory
    const ramfs_entry_t* e=ramfs_find(path); if(!e) return -1; return (e->flags & 2)?1:0;
}
size_t ramfs_list_path(const char* path, const ramfs_entry_t** out_array, size_t max){
    // List only direct children of path (root: path=="" or "/" -> all top-level entries lacking '/').
    int root = 0; if(!path || path[0]==0 || (path[0]=='/' && path[1]==0)) root=1; else { if(ramfs_is_dir(path)!=1) return 0; }
    size_t count=0;
    for(size_t i=0;i<ramfs_count && count<max;i++){
        const char* name = ramfs_table[i].name;
    if(root){ // must not contain '/'
            const char* slash = 0; for(size_t k=0; name[k]; k++){ if(name[k]=='/'){ slash=&name[k]; break; } }
            if(!slash){ out_array[count++]=&ramfs_table[i]; }
        } else {
            size_t plen=str_len(path);
            if(str_starts(name,path) && name[plen]=='/'){
                // child or descendant; accept only direct child (no additional '/').
                const char* sub = name+plen+1; int has_slash=0; for(size_t k=0; sub[k]; k++){ if(sub[k]=='/'){ has_slash=1; break; } }
                if(!has_slash) out_array[count++]=&ramfs_table[i];
            }
        }
    }
    return count;
}

static int path_validate_parent(const char* path){ // verify each parent component exists and is a directory
    for(size_t i=0; path[i]; i++){
        if(path[i]=='/'){
            // parent = substring path[0..i-1]
            char parent[RAMFS_NAME_MAX]; if(i>=RAMFS_NAME_MAX) return 0; for(size_t k=0;k<i;k++) parent[k]=path[k]; parent[i]=0;
            const ramfs_entry_t* pe = ramfs_find(parent); if(!pe || !(pe->flags & 2)) return 0; // parent must be directory
        }
    }
    return 1;
}
int ramfs_mkdir(const char* path){
    if(!path || !*path) return -1; if(str_len(path)>=RAMFS_NAME_MAX) return -1; if(ramfs_find(path)) return -1; if(!path_validate_parent(path)) return -1; // parent validation
    return ramfs_add_common(path,"",0,2); // directory flag
}
int ramfs_rmdir(const char* path){
    ramfs_entry_t* e = (ramfs_entry_t*)ramfs_find(path); if(!e) return -1; if(!(e->flags & 2)) return -1; if(e->flags & 1) return -1; // immutable directory not removable
    // ensure empty: no entries whose name starts with path + '/'
    size_t plen=str_len(path);
    for(size_t i=0;i<ramfs_count;i++){
        if(ramfs_table[i].name!=e->name){
            if(str_starts(ramfs_table[i].name,path) && ramfs_table[i].name[plen]=='/') return -1; // has child
        }
    }
    // reuse remove
    return ramfs_remove(path);
}

int ramfs_rename(const char* old_path, const char* new_path){
    if(!old_path || !new_path || !*old_path || !*new_path) return -1;
    if(str_len(new_path) >= RAMFS_NAME_MAX) return -1;
    if(ramfs_find(new_path)) return -1; // target already exists
    ramfs_entry_t* e = (ramfs_entry_t*)ramfs_find(old_path); if(!e) return -1; if(e->flags & 1) return -1; // immutable cannot be renamed
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
    if(slash_last){ size_t plen = (size_t)(slash_last - new_path); char parent[RAMFS_NAME_MAX]; for(size_t i=0;i<plen && i<RAMFS_NAME_MAX-1;i++) parent[i]=new_path[i]; parent[plen]=0; const ramfs_entry_t* pe = ramfs_find(parent); if(!pe || !(pe->flags & 2)) return -1; }
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

int ramfs_init(void){ if(ramfs_inited) return 0; ramfs_inited=1; // sample immutable files & dirs
    static const char readme_txt[] = "SecOS RAMFS\nThis is a demonstrative in-memory filesystem.\n";
    static const char hello_txt[] = "Hello from RAMFS!\n";
    ramfs_add_static("README.txt", readme_txt, sizeof(readme_txt)-1);
    ramfs_add_static("hello.txt", hello_txt, sizeof(hello_txt)-1);
    // Example mutable directory
    ramfs_mkdir("docs");
    ramfs_add("docs/info.txt","Documentazione RAMFS dir docs",29);
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
    size_t vp=0; // scrittura sicura
    const char* parts[] = {"VERSION=", basev, "\nBUILD_TS=", ts, "\nGIT_HASH=", gh, "\n"};
    for(int pi=0; pi<7; pi++){ const char* s=parts[pi]; for(size_t k=0; s[k] && vp<sizeof(version_buf)-1; k++) version_buf[vp++]=s[k]; }
    version_buf[vp]=0;
    ramfs_add_static("VERSION", version_buf, vp);
    // Syscalls listing (placeholder)
    static const char syscalls_txt[] = "0 exit\n1 write\n2 read\n3 sleep\n"; ramfs_mkdir("sys"); ramfs_add_static("sys/syscalls.txt", syscalls_txt, sizeof(syscalls_txt)-1);
    // Init script: each line a shell command
    static const char init_rc_txt[] = "# init.rc SecOS\ncolor light_gray black\nrfusage\nrftree\nrfcat sys/manifest.txt\n"; ramfs_add_static("init.rc", init_rc_txt, sizeof(init_rc_txt)-1);
    // Dynamic manifest: list initial entries (after base creation) under sys/manifest.txt
    {
        static char manifest_buf[4096]; size_t mp=0;
    const char* hdr = "# SecOS RAMFS Manifest\n# List of initial entries\n"; for(size_t i=0; hdr[i] && mp<sizeof(manifest_buf)-1; i++) manifest_buf[mp++]=hdr[i];
        const ramfs_entry_t* arr[RAMFS_MAX_FILES]; size_t n = ramfs_list(arr,RAMFS_MAX_FILES);
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
        ramfs_add_static("sys/manifest.txt", manifest_buf, mp);
    }
    return 0; }
