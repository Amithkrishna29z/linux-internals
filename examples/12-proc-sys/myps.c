/* myps.c
 *
 * A miniature `ps`: walk /proc, and for every entry that is a PID directory,
 * read /proc/[pid]/status to extract the name, state, PPID, and resident
 * memory, then print a table. This is essentially how ps/top/htop work --
 * they parse /proc. No special API, just readdir + fopen + fgets.
 *
 * Compile:  gcc -Wall -Wextra -o myps myps.c
 * Run:      ./myps
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

/* Pull one "Key:\tvalue" field out of a status file into `out`. */
static int field(const char *path, const char *key, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int found = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) == 0) {
            /* skip the key and any leading whitespace */
            char *v = line + klen;
            while (*v == ':' || *v == ' ' || *v == '\t') v++;
            v[strcspn(v, "\n")] = '\0';
            snprintf(out, outsz, "%s", v);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

int main(void)
{
    DIR *d = opendir("/proc");
    if (!d) { perror("opendir /proc"); return 1; }

    printf("%7s  %-20s  %-5s  %10s  %s\n", "PID", "NAME", "STATE", "RSS(kB)", "PPID");

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* A process directory is named entirely of digits. */
        if (!isdigit((unsigned char)e->d_name[0])) continue;

        char path[300];
        snprintf(path, sizeof path, "/proc/%s/status", e->d_name);

        char name[64] = "?", state[16] = "?", rss[16] = "0", ppid[16] = "?";
        if (field(path, "Name", name, sizeof name) < 0)
            continue;                       /* process vanished between readdir and open */
        field(path, "State", state, sizeof state);
        field(path, "VmRSS", rss, sizeof rss);   /* kernel threads have no VmRSS */
        field(path, "PPid",  ppid, sizeof ppid);

        /* State is like "R (running)"; keep just the letter. */
        char st = state[0];
        printf("%7s  %-20s  %-5c  %10s  %s\n", e->d_name, name, st, rss, ppid);
    }

    closedir(d);
    return 0;
}
