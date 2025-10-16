#ifndef SHELL_H
#define SHELL_H

// Inizializza e avvia la shell
void shell_init(void);
void shell_run(void);
// Esegue una singola linea di comando (usata da init.rc)
void shell_run_line(const char* line);

#endif