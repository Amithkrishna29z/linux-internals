/* ifaddr.c
 *
 * Lists the machine's network interfaces and their IPv4 addresses using the
 * classic ioctl interface (SIOCGIFCONF + SIOCGIFADDR) -- the same calls the
 * old `ifconfig` used. Issued on an ordinary socket fd, which acts as the
 * handle to the kernel's networking. (Modern `ip` uses netlink instead.)
 *
 * Compile:  gcc -Wall -Wextra -o ifaddr ifaddr.c
 * Run:      ./ifaddr
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>         /* struct ifconf, struct ifreq */
#include <arpa/inet.h>

int main(void)
{
    /* Any socket works as the handle for network ioctls. */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    /* SIOCGIFCONF fills a buffer with an array of struct ifreq (one per iface). */
    struct ifreq ifrs[32];
    struct ifconf ifc;
    ifc.ifc_len = sizeof ifrs;
    ifc.ifc_req = ifrs;
    if (ioctl(s, SIOCGIFCONF, &ifc) < 0) { perror("ioctl(SIOCGIFCONF)"); return 1; }

    int count = ifc.ifc_len / sizeof(struct ifreq);
    printf("found %d interface(s):\n", count);

    for (int i = 0; i < count; i++) {
        struct ifreq ifr = ifrs[i];       /* copy: the next ioctl overwrites fields */

        /* SIOCGIFADDR: get THIS interface's IPv4 address into ifr.ifr_addr. */
        if (ioctl(s, SIOCGIFADDR, &ifr) < 0) {
            printf("  %-10s  (no IPv4 address)\n", ifrs[i].ifr_name);
            continue;
        }
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);

        /* SIOCGIFFLAGS: is it up? */
        char *state = "";
        if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0)
            state = (ifr.ifr_flags & IFF_UP) ? "UP" : "DOWN";

        printf("  %-10s  %-15s  %s\n", ifrs[i].ifr_name, ip, state);
    }

    close(s);
    return 0;
}
