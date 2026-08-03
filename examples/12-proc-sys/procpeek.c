/* procpeek.c
 *
 * Reads the process's OWN /proc entries: /proc/self/status (selected human-
 * readable fields) and /proc/self/maps (the live virtual-memory segments --
 * the concrete version of Module 7's text/data/heap/stack diagram). No special
 * API: /proc files are opened and read like any other file.
 *
 * Compile:  gcc -Wall -Wextra -o procpeek procpeek.c
 * Run:      ./procpeek
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print lines from /proc/self/status whose key is one we care about. */
static void show_status(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) { perror("open status"); return; }

    char line[256];
    const char *want[] = { "Name:", "Pid:", "PPid:", "VmRSS:", "VmSize:",
                           "Threads:", "State:", NULL };
    printf("== /proc/self/status ==\n");
    while (fgets(line, sizeof line, f)) {
        for (int i = 0; want[i]; i++)
            if (strncmp(line, want[i], strlen(want[i])) == 0)
                fputs(line, stdout);
    }
    fclose(f);
}

/* Print the memory map: each line is  addr-range perms offset dev inode  path */
static void show_maps(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { perror("open maps"); return; }

    printf("\n== /proc/self/maps (virtual memory segments) ==\n");
    char line[512];
    while (fgets(line, sizeof line, f)) {
        /* Highlight the interesting named regions: heap, stack, our binary. */
        if (strstr(line, "[heap]") || strstr(line, "[stack]") ||
            strstr(line, "procpeek") || strstr(line, "[vdso]"))
            fputs(line, stdout);
    }
    fclose(f);
}

int main(void)
{
    /* Allocate something so [heap] is populated and visible in maps. */
    char *p = malloc(1 << 20);   /* 1 MB */
    if (p) p[0] = 'x';            /* touch it so a page is backed (Module 7) */

    show_status();
    show_maps();

    free(p);
    return 0;
}
