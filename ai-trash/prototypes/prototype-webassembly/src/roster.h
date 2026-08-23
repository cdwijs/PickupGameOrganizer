#ifndef ROSTER_H
#define ROSTER_H
#include "base.h"

int roster_load(const char *text, const char *username);
int roster_block_count(void);
int roster_filled(int idx);
int roster_going(int idx);
const char *roster_date(int idx);
const char *roster_weekday(int idx);
const char *roster_time(void);
void roster_toggle(int idx);
const char *roster_text(void);
void roster_capitalize(char *dst, usize cap, const char *name);
void roster_display_name(char *dst, usize cap, const char *username);
#endif
