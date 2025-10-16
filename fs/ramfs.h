#ifndef RAMFS_H
#define RAMFS_H
#include <stdint.h>
#include <stddef.h>

// Semplice RAM filesystem immutabile (montato in memoria) con tabella fissa.
// Limiti: max 32 file, nome <= 32 chars, contenuto in memoria kernel.

#define RAMFS_MAX_FILES 32
#define RAMFS_NAME_MAX  96 // aumenta per percorsi con sottodirectory

// flags: bit0 immutable, bit1 directory
typedef struct {
    char     name[RAMFS_NAME_MAX]; // path completo (es: "dir/sub/file") oppure semplice nome root
    uint8_t* data; // file data (NULL per directory)
    size_t   size; // size file (0 per directory)
    unsigned flags; // bit0 immutable, bit1 directory
} ramfs_entry_t;

int ramfs_init(void); // prepara tabella e registra file statici
const ramfs_entry_t* ramfs_find(const char* name);
size_t ramfs_list(const ramfs_entry_t** out_array, size_t max);
int ramfs_add(const char* name, const void* data, size_t size); // aggiunge file (mutable)
int ramfs_add_static(const char* name, const void* data, size_t size); // aggiunge file immutabile (init)
int ramfs_write(const char* name, size_t offset, const void* src, size_t len); // ritorna bytes scritti o -1
int ramfs_truncate(const char* name, size_t new_size); // -1 errore
int ramfs_remove(const char* name); // -1 errore (non trovato / immutabile)
// Directory API
int ramfs_mkdir(const char* path); // crea directory (mutable, vuota)
int ramfs_rmdir(const char* path); // rimuove directory se vuota (non immutabile)
size_t ramfs_list_path(const char* path, const ramfs_entry_t** out_array, size_t max); // lista contenuti della directory indicata
int ramfs_is_dir(const char* path); // 1 se directory, 0 se file, -1 se non esiste
int ramfs_rename(const char* old_path, const char* new_path); // rinomina file o directory (aggiorna figli se directory non vuota)

#endif
