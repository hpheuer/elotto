/* Wire format for the node link — see include/elotto_link.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elotto_link.h"

int elotto_link_pack(char *buf, int cap, uint32_t seq, const char *payload)
{
    return snprintf(buf, cap, ELOTTO_LINK_MAGIC " %lu %s",
                    (unsigned long)seq, payload);
}

bool elotto_link_parse(char *msg, uint32_t *seq, char **payload)
{
    const size_t ml = sizeof(ELOTTO_LINK_MAGIC) - 1;
    if (strncmp(msg, ELOTTO_LINK_MAGIC, ml) != 0 || msg[ml] != ' ') return false;

    char *p = msg + ml + 1;
    char *end = NULL;
    unsigned long s = strtoul(p, &end, 10);
    if (end == p || *end != ' ') return false;

    *seq     = (uint32_t)s;
    *payload = end + 1;
    return **payload != '\0';
}
