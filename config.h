#ifndef CONFIG_H
#define CONFIG_H

// Toggle di funzionalità principali
#define ENABLE_PMM      1      // Gestione memoria fisica
#define ENABLE_VMM      1      // Gestione memoria virtuale / paging
#define ENABLE_TSS      1      // Task State Segment e stack privilegi
#define ENABLE_HEAP     1      // Allocatore heap
#define ENABLE_SHELL    1      // Shell interattiva
#define ENABLE_RTC      1      // TODO: Real Time Clock driver
#define ENABLE_FB       1      // Console framebuffer grafica (richiede tag multiboot framebuffer o GOP UEFI)

// Modalità di boot (impostare esattamente una a 1)
#define CONFIG_MULTIBOOT 1     // Avvio legacy via GRUB Multiboot/Multiboot2
#define CONFIG_UEFI      1     // Avvio moderno via bootloader UEFI esterno

// Nota: entrambi possono essere 1 per supporto dual-boot
#if (CONFIG_MULTIBOOT == 0 && CONFIG_UEFI == 0)
#error "Devi abilitare almeno una modalità di boot: CONFIG_MULTIBOOT o CONFIG_UEFI"
#endif

// Logging verboso / debug
#define ENABLE_DEBUG_LOG 0

#endif // CONFIG_H
