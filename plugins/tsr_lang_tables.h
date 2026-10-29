/* Text language tables for the USA build (SLUS-01214). The game's program
 * carries every text in six languages, six pointers in a row (English,
 * French, German, Italian, Spanish, Dutch); the European build picks one
 * through its language byte, the American one always reads the first.
 * tools/make_us_project.py writes the American tables here (their address
 * and the English pointer they hold); the European build lists none. */
#ifndef TSR_LANG_TABLES_H
#define TSR_LANG_TABLES_H
#include <stdint.h>

typedef struct { uint32_t addr, en; } TsrLangTable;

#define TSR_LANG_TABLE_COUNT 0
static const TsrLangTable TSR_LANG_TABLES[1] = { { 0u, 0u } };

#endif
