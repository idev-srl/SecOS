#ifndef CONFIG_H
#define CONFIG_H

// Feature toggles
#define ENABLE_PMM      1
#define ENABLE_VMM      1
#define ENABLE_TSS      1
#define ENABLE_HEAP     1
#define ENABLE_SHELL    1
#define ENABLE_RTC      1   // Da implementare
#define ENABLE_FB       1   // Framebuffer grafico attivo (richiede header multiboot con richiesta framebuffer)

// Verbose logging
#define ENABLE_DEBUG_LOG 0

#endif // CONFIG_H
