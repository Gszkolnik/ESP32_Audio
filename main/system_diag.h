#ifndef SYSTEM_DIAG_H
#define SYSTEM_DIAG_H

// Returns JSON string with system diagnostics. Caller must free().
char* system_diag_get_json(void);

#endif
