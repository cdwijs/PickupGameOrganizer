#ifndef JSON_H
#define JSON_H
#include "base.h"

enum { J_NULL = 0, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct JVal {
  int type;
  long long num;
  char *str;
  char *key;
  struct JVal *first;
  struct JVal *next;
} JVal;

JVal *json_parse(const char *text, usize len);
JVal *json_get(const JVal *obj, const char *key);
const char *json_str(const JVal *obj, const char *key, const char *fallback);
long long json_num(const JVal *obj, const char *key, long long fallback);
int json_count(const JVal *arr);
#endif
