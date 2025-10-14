#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

// Inizializza la tastiera
void keyboard_init(void);

// Handler interrupt (chiamato da assembly)
void keyboard_handler(void);

// Leggi un carattere (bloccante)
char keyboard_getchar(void);

// Controlla se c'è un carattere disponibile
bool keyboard_has_char(void);

// Leggi una riga (fino a Enter)
void keyboard_readline(char* buffer, int max_len);

#endif