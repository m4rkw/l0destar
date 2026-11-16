/*
 * Installation mode (APP_INSTALL_MODE): a unit being fitted may have no 12 V
 * and no antenna yet, so it keeps a connection, polls the server every 10 s
 * for `instreport` / `instenable`, and raises no alerts.  See install.c.
 */
#ifndef INSTALL_H_
#define INSTALL_H_

#include <stdbool.h>
#include <zephyr/kernel.h>
#include "install_core.h"

#if IS_ENABLED(CONFIG_APP_INSTALL_MODE)

/* Parts whose bring-up can fail without stopping the rest (noted, kept in RAM,
 * never alerted).  main.c reports them as it goes. */
enum install_part { INSTALL_PART_MODEM, INSTALL_PART_TLS, INSTALL_PART_GNSS,
                    INSTALL_PART_COUNT };

void install_init(void);                    /* once, early in main() */
bool install_active(void);
void install_set(bool on, const char *why);
void install_note(enum install_part p, bool ok);
/* In cmd_run: takes any im=<0|1> out of the reply and follows it. */
void install_take_token(char *cmd);
void install_request_report(void);          /* command instreport */
void install_request_enable(void);          /* command instenable */
/* Appended to every record's tail by data.c: im=1 while in the mode, ir= and
 * ie= once each.  Returns the bytes written; install_fields_taken() once the
 * text is in a record. */
int  install_fields(char *buf, size_t size);
void install_fields_taken(void);
/* The mode's loop; returns when the mode is switched off.  `retry` (may be
 * NULL) redoes whatever bring-up step failed at boot. */
void install_run(void (*retry)(void));

#else

enum install_part { INSTALL_PART_MODEM, INSTALL_PART_TLS, INSTALL_PART_GNSS,
                    INSTALL_PART_COUNT };
static inline void install_init(void) { }
static inline bool install_active(void) { return false; }
static inline void install_set(bool on, const char *why) { }
static inline void install_note(enum install_part p, bool ok) { }
/* The server still sends im= on every reply: take it out so the rest of the
 * reply is what cmd_run always saw. */
static inline void install_take_token(char *cmd) { (void)install_take_im(cmd); }
static inline void install_request_report(void) { }
static inline void install_request_enable(void) { }
static inline int  install_fields(char *buf, size_t size) { return 0; }
static inline void install_fields_taken(void) { }
static inline void install_run(void (*retry)(void)) { }

#endif
#endif
