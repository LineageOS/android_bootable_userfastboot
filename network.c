#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <netinet/ether.h>
#include <linux/rtnetlink.h>
#include <pthread.h>
#include <unistd.h>

#include "userfastboot_ui.h"
#include "userfastboot_util.h"
#include "aboot.h"

static int do_network_ioctl(int fd, int request, char *name, struct ifreq *ifr)
{
	memset(ifr, 0, sizeof(*ifr));
	strncpy(ifr->ifr_name, name, IFNAMSIZ-1);
	return ioctl(fd, request, ifr);
}

static char *get_ip_string(int fd, char *name)
{
	struct ifreq ifr;
	struct sockaddr_in *sin;

	if (do_network_ioctl(fd, SIOCGIFADDR, name, &ifr)) {
		pr_perror("SIOCGIFADDR");
		return NULL;
	}
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	return xasprintf("%s", inet_ntoa(sin->sin_addr));
}

static char *get_mac_string(int fd, char *name)
{
	struct ifreq ifr;
	struct ether_addr *ethaddr;

	if (do_network_ioctl(fd, SIOCGIFHWADDR, name, &ifr)) {
		pr_perror("SIOCGIFHWADDR");
		return NULL;
	}
	ethaddr = (struct ether_addr *)&ifr.ifr_hwaddr.sa_data;
	return xasprintf("%s", ether_ntoa(ethaddr));
}

char *get_network_interface_status(void)
{
	struct ifreq ifaces[16];
	struct ifconf ifconf;
	int ctr = 0;
	int fd, i;
	char *outstr = NULL;

	fd = socket(PF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		pr_perror("socket");
		return NULL;
	}

	ifconf.ifc_req = ifaces;
	ifconf.ifc_len = sizeof(ifaces);

	if (ioctl(fd, SIOCGIFCONF, &ifconf)) {
		pr_perror("SIOCGIFCONF");
		close(fd);
		return NULL;
	}

	outstr = xstrdup("");

	for (i = 0, ctr = 0; i < ifconf.ifc_len; i += sizeof(struct ifreq), ctr++) {
		char *name, *ip, *mac, *old_info;
		struct ifreq *iface = &ifaces[ctr];

		name = iface->ifr_name;

		if (!strcmp(name, "lo"))
			continue;

		ip = get_ip_string(fd, name);
		mac = get_mac_string(fd, name);

		old_info = outstr;
		outstr = xasprintf("%s %s %s\n%s", name, ip, mac, old_info);
		free(ip);
		free(mac);
		free(old_info);
	}
	close(fd);

	return outstr;
}


void *interface_thread(void *data)
{
	int fd;
	struct sockaddr_nl sa;
	ssize_t len;
	char buf[4096];
	struct iovec iov = { buf, sizeof(buf) };

	/* Open up the socket */
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_IPV4_IFADDR;
	sa.nl_pid = getpid();

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0) {
		pr_perror("socket");
		return NULL;
	}

	if (bind(fd, (struct sockaddr *) &sa, sizeof(sa))) {
		pr_perror("bind");
		goto out;
	}

	/* Initial UI update; the socket is open so we won't miss
	 * anything */
	populate_status_info();

	while (1) {
		struct msghdr msg = { &sa, sizeof(sa), &iov, 1, NULL, 0, 0 };
		len = recvmsg(fd, &msg, 0);
		if (len < 0) {
			pr_perror("recvmsg");
			goto out;
		}

		/* We don't bother looking at the message, we only listen for
		 * RTMGRP_IPV4_IPADDR so just repop the status */
		pr_debug("got netlink event\n");
		populate_status_info();
	}

out:
	close(fd);
	return NULL;
}


void start_interface_thread(void)
{
	pthread_t t;
	pthread_create(&t, NULL, interface_thread, NULL);
}

