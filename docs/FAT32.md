# Piano Driver FAT32 (Bozza)

## Obiettivi Iniziali
- Implementare supporto **read-only** FAT32 per montare un volume e leggere file/directory.
- Integrare con il layer VFS esistente (inodes virtuali, readdir, read file).

## Componenti Necessari
1. **Block Device Astratto**: `int blk_read(uint64_t lba, void* buf, size_t sectors)` provvisorio su buffer in RAM.
2. **Parser BPB (BIOS Parameter Block)**: Estrarre valori chiave:
   - Bytes per settore
   - Settori per cluster
   - Numero FAT
   - Settori riservati
   - Settori per FAT
   - Primo cluster root (FAT32 usa cluster root invece di root directory fissa)
3. **Gestione FAT Table**: Caricamento lazy di entry FAT per seguire catena cluster di un file.
4. **Directory Entries** (short name + LFN): Per la prima versione ignorare LFN e usare solo 8.3; successivamente aggiungere parsing LFN.
5. **Caching Clusters**: Buffer temporaneo per cluster corrente (ottimizzazione semplice; no LRU avanzato all'inizio).
6. **Traduzione Path -> cluster**: Walk directory componenti usando confronto 8.3 case-insensitive.

## Mappatura in VFS
- Ogni file/directory inode contiene:
  - Primo cluster
  - Size (per file)
  - Flag directory
- `lookup(path)`: risolve partendo da root cluster.
- `readdir(path)`: elenca entries nel cluster chain della directory.
- `read(file, offset, buf, len)`: segue catena FAT fino al cluster contenente offset.

## Limiti Versione Iniziale
- Niente scrittura/modifica/metadati estesi.
- Ignora attributi avanzati (solo DIR/FILE, hidden ignorato).
- Niente timestamps.
- Nessun supporto LFN nella prima iterazione.

## Estensioni Future
- Supporto LFN (sequenza di entries con attribute 0x0F).
- Write support (allocazione cluster libera, aggiornamento FAT, creazione entries).
- Cache multi-cluster + prefetch.
- Gestione attributi (RO, H, S, A).
- Timestamps DosDate -> conversione.

## exFAT Considerazioni
exFAT differisce radicalmente (allocation bitmap, directory entries variabili, upcase table). Verrà affrontato dopo che FAT32 base è stabile.

## Sequenza Implementazione
1. Stub block device (buffer fittizio con immagine FAT32 caricata staticamente).
2. Lettura BPB e validazione signature.
3. Caricamento primo cluster root e list readdir.
4. Implementazione lookup componenti.
5. Implementazione read file linearizzata.
6. Integrazione con VFS mount (es. `vfs_mount_fat32(image_base)`).
7. Test: elenco root, cat di un file noto.

## Test Pianificati
- Avvia kernel con immagine FAT32 embedded e confrontare output `vls /` vs entries attese.
- `vcat /README.TXT` corrisponde al contenuto noto.

## Note Performance
La versione iniziale sacrifica performance per semplicità; cluster traversal lineare per ogni read.

---
Bozza – soggetta a modifiche in base alle esigenze.
