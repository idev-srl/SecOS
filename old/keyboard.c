#include "keyboard.h"

#define KEYBOARD_DATA_PORT 0x60
#define BUFFER_SIZE 256

// Buffer circolare per l'input
static char input_buffer[BUFFER_SIZE];
static int buffer_start = 0;
static int buffer_end = 0;

// Stati dei tasti modificatori
static bool shift_pressed = false;
static bool caps_lock = false;

// Funzioni I/O inline
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Mappa scancode US QWERTY -> ASCII (senza shift)
static const char scancode_to_ascii[] = {
    0,  0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

// Mappa scancode US QWERTY -> ASCII (con shift)
static const char scancode_to_ascii_shift[] = {
    0,  0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

// Aggiungi carattere al buffer
static void buffer_put(char c) {
    int next = (buffer_end + 1) % BUFFER_SIZE;
    if (next != buffer_start) {
        input_buffer[buffer_end] = c;
        buffer_end = next;
    }
}

// Leggi carattere dal buffer
static char buffer_get(void) {
    if (buffer_start == buffer_end) {
        return 0;
    }
    char c = input_buffer[buffer_start];
    buffer_start = (buffer_start + 1) % BUFFER_SIZE;
    return c;
}

// Controlla se il buffer ha caratteri
bool keyboard_has_char(void) {
    return buffer_start != buffer_end;
}

// Handler interrupt tastiera
void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Gestisci key release (bit 7 = 1)
    if (scancode & 0x80) {
        scancode &= 0x7F;
        // Shift release
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = false;
        }
        return;
    }
    
    // Gestisci tasti speciali
    if (scancode == 0x2A || scancode == 0x36) {  // Left/Right Shift
        shift_pressed = true;
        return;
    }
    
    if (scancode == 0x3A) {  // Caps Lock
        caps_lock = !caps_lock;
        return;
    }
    
    // Converti scancode in ASCII
    char ascii = 0;
    if (scancode < sizeof(scancode_to_ascii)) {
        if (shift_pressed) {
            ascii = scancode_to_ascii_shift[scancode];
        } else {
            ascii = scancode_to_ascii[scancode];
            // Gestisci Caps Lock per le lettere
            if (caps_lock && ascii >= 'a' && ascii <= 'z') {
                ascii -= 32;  // Converti in maiuscolo
            }
        }
    }
    
    if (ascii) {
        buffer_put(ascii);
    }
}

// Inizializza la tastiera
void keyboard_init(void) {
    buffer_start = 0;
    buffer_end = 0;
    shift_pressed = false;
    caps_lock = false;
}

// Leggi un carattere (bloccante)
char keyboard_getchar(void) {
    while (!keyboard_has_char()) {
        __asm__ volatile ("hlt");  // Attendi interrupt
    }
    return buffer_get();
}

// Leggi una riga completa
void keyboard_readline(char* buffer, int max_len) {
    int pos = 0;
    
    while (pos < max_len - 1) {
        char c = keyboard_getchar();
        
        if (c == '\n') {
            buffer[pos] = '\0';
            return;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
            }
        } else {
            buffer[pos++] = c;
        }
    }
    
    buffer[pos] = '\0';
}