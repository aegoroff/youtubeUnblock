#define _GNU_SOURCE
#ifndef __linux__
#error "The package is linux only!"
#endif

#ifdef KERNEL_SPACE
#error "The build aims to the kernel, not userspace"
#endif

#include <libnetfilter_queue/linux_nfnetlink_queue.h>
#include <stdio.h>
#include <stdlib.h>

#include <libmnl/libmnl.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <libnetfilter_queue/libnetfilter_queue_ipv4.h>
#include <libnetfilter_queue/libnetfilter_queue_tcp.h>
#include <libnetfilter_queue/libnetfilter_queue_udp.h>
#include <libnetfilter_queue/pktbuff.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <linux/if_ether.h>
#include <linux/netfilter.h>
#include <pthread.h>
#include <sys/socket.h>

#include "config.h"
#include "mangle.h"

pthread_mutex_t rawsocket_lock;

struct config_t config = {
	.rawsocket = -2,
	.threads = THREADS_NUM,
	.fragmentation_strategy = FRAGMENTATION_STRATEGY,
	.fake_sni_strategy = FAKE_SNI_STRATEGY,
	.fake_sni_ttl = FAKE_SNI_TTL,

#ifdef SEG2_DELAY
	.seg2_delay = SEG2_DELAY,
#else
	.seg2_delay = 0,
#endif

#ifdef USE_GSO
	.use_gso = true,
#else
	.use_gso = false,
#endif

#ifdef DEBUG
	.verbose = true,
#else
	.verbose = false,
#endif
	.domains_str = defaul_snistr,
	.domains_strlen = sizeof(defaul_snistr),
};

const char* get_value(const char *option, const char *prefix)
{
	errno = 0;

	size_t prefix_len = strlen(prefix);
	size_t option_len = strlen(option);

	if (option_len <= prefix_len || strncmp(prefix, option, prefix_len)) {
		return NULL;
	}

	return option + prefix_len;
}

int parse_bool_option(const char *value) {
	errno = 0;
	if (strcmp(value, "1") == 0) {
		return 1;
	}
	else if (strcmp(value, "0") == 0) {
		return 0;
	}

	errno = EINVAL;
	return -1;
}

long parse_numeric_option(const char* value) {
	errno = 0;

	char* end;
	long result = strtol(value, &end, 10);
	if (*end != '\0') {
		errno = EINVAL;
		return 0;
	}

	return result;
}

int parse_option(const char* option) {
	const char* value;
	int ret;

	if (!strcmp(option, "--no-gso")) {
		config.use_gso = 0;
		goto out;
	}

	if (!strcmp(option, "--silent")) {
		config.verbose = 0;
		goto out;
	}

	if ((value = get_value(option, "--sni-domains")) != 0) {
		if (!value) {
			goto err;
		}
		
		if (strcmp(value, "all")) {
			config.all_domains = 1;
		}

		config.domains_str = value;
		config.domains_strlen = strlen(value);

		goto out;
	}

	if ((value = get_value(option, "--frag=")) != 0) {
		if (!value) {
			goto err;
		}

		if (strcmp(value, "tcp") == 0) {
			config.fragmentation_strategy = FRAG_STRAT_TCP;
		} else if (strcmp(value, "ip") == 0) {
			config.fragmentation_strategy = FRAG_STRAT_IP;
		} else if (strcmp(value, "none") == 0) {
			config.fragmentation_strategy = FRAG_STRAT_NONE;
		} else {
			goto err;
		}

		goto out;
	}

	if ((value = get_value(option, "--fake-sni=")) != 0) {
		if (strcmp(value, "ack") == 0) {
			config.fake_sni_strategy = FKSN_STRAT_ACK_SEQ;
		} else if (strcmp(value, "ttl") == 0) {
			config.fake_sni_strategy = FKSN_STRAT_TTL;
		}
		else if (strcmp(value, "none") == 0) {
			config.fake_sni_strategy = FKSN_STRAT_NONE;
		} else {
			goto err;
		}

		goto out;
	}

	if ((value = get_value(option, "--seg2delay=")) != 0) {
		long num = parse_numeric_option(value);
		if (errno != 0 ||
			num < 0)
			goto err;

		config.seg2_delay = num;
		goto out;
	}

	if ((value = get_value(option, "--threads=")) != 0) {
		long num = parse_numeric_option(value);
		if (errno != 0 ||
			num < 0 ||
			num > MAX_THREADS) {
			errno = EINVAL;
			goto err;
		}

		config.threads = num;
		goto out;
	}

	if ((value = get_value(option, "--fake-sni-ttl=")) != 0) {
		long num = parse_numeric_option(value);
		if (errno != 0 ||
			num < 0 ||
			num > 255) {
			goto err;
		}

		config.fake_sni_ttl = num;
		goto out;
	}

err:
	errno = EINVAL;
	return -1;
out:
	errno = 0;
	return 0;
}

static int parse_args(int argc, const char *argv[]) {
	int err;
	char *end;

	if (argc < 2) {
		errno = EINVAL;
		goto errormsg_help;
	}

	config.queue_start_num = parse_numeric_option(argv[1]);
	if (errno != 0) goto errormsg_help;

	for (int i = 2; i < argc; i++) {
		if (parse_option(argv[i])) {
			printf("Invalid option %s\n", argv[i]);
			goto errormsg_help;
		}
	}

	return 0;

errormsg_help:
	err = errno;
	printf("Usage: %s <queue_num> [OPTIONS]\n", argv[0]);
	printf("Options:\n");
	printf("\t--sni-domains=<comma separated domain list>|all\n");
	printf("\t--fake-sni={ack,ttl,none}\n");
	printf("\t--fake-sni-ttl=<ttl>\n");
	printf("\t--frag={tcp,ip,none}\n");
	printf("\t--seg2delay=<delay>\n");
	printf("\t--threads=<threads number>\n");
	printf("\t--silent\n");
	printf("\t--no-gso\n");
	errno = err;
	if (errno == 0) errno = EINVAL;

	return -1;
}

static int open_socket(struct mnl_socket **_nl) {
	struct mnl_socket *nl = NULL;
	nl = mnl_socket_open(NETLINK_NETFILTER);

	if (nl == NULL) {
		perror("mnl_socket_open");
		return -1;
	}

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		perror("mnl_socket_bind");
		mnl_socket_close(nl);
		return -1;
	}

	*_nl = nl;

	return 0;
}


static int close_socket(struct mnl_socket **_nl) {
	struct mnl_socket *nl = *_nl;
	if (nl == NULL) return 1;
	if (mnl_socket_close(nl) < 0) {
		perror("mnl_socket_close");
		return -1;
	}

	*_nl = NULL;

	return 0;
}

static int open_raw_socket(void) {
	if (config.rawsocket != -2) {
		errno = EALREADY;
		perror("Raw socket is already opened");
		return -1;
	}
	
	config.rawsocket = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	if (config.rawsocket == -1) {
		perror("Unable to create raw socket");
		return -1;
	}

	int mark = RAWSOCKET_MARK;
	if (setsockopt(config.rawsocket, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) < 0)
	{
		fprintf(stderr, "setsockopt(SO_MARK, %d) failed\n", mark);
		return -1;
	}

	int mst = pthread_mutex_init(&rawsocket_lock, NULL);
	if (mst) {
		fprintf(stderr, "Mutex err: %d\n", mst);
		close(config.rawsocket);
		errno = mst;

		return -1;
	}


	return config.rawsocket;
}

static int close_raw_socket(void) {
	if (config.rawsocket < 0) {
		errno = EALREADY;
		perror("Raw socket is not set");
		return -1;
	}

	if (close(config.rawsocket)) {
		perror("Unable to close raw socket");
		pthread_mutex_destroy(&rawsocket_lock);
		return -1;
	}

	pthread_mutex_destroy(&rawsocket_lock);

	config.rawsocket = -2;
	return 0;
}


static int send_raw_socket(const uint8_t *pkt, uint32_t pktlen) {
	int ret;

	if (pktlen > AVAILABLE_MTU) {
		if (config.verbose)
			printf("Split packet!\n");

		uint8_t buff1[MNL_SOCKET_BUFFER_SIZE];
		uint32_t buff1_size = MNL_SOCKET_BUFFER_SIZE;
		uint8_t buff2[MNL_SOCKET_BUFFER_SIZE];
		uint32_t buff2_size = MNL_SOCKET_BUFFER_SIZE;

		switch (config.fragmentation_strategy) {
			case FRAG_STRAT_TCP:
				if ((ret = tcp4_frag(pkt, pktlen, AVAILABLE_MTU-128,
					buff1, &buff1_size, buff2, &buff2_size)) < 0) {

					errno = -ret;
					return ret;
				}
				break;
			case FRAG_STRAT_IP:
				if ((ret = ip4_frag(pkt, pktlen, AVAILABLE_MTU-128,
					buff1, &buff1_size, buff2, &buff2_size)) < 0) {

					errno = -ret;
					return ret;
				}
				break;
			default:
				errno = EINVAL;
				printf("send_raw_socket: Packet is too big but fragmentation is disabled!\n");
				return -EINVAL;
		}

		int sent = 0;
		int status = send_raw_socket(buff1, buff1_size);

		if (status >= 0) sent += status;
		else {
			return status;
		}

		status = send_raw_socket(buff2, buff2_size);
		if (status >= 0) sent += status;
		else {
			return status;
		}

		return sent;
	}


	struct iphdr *iph;

	if ((ret = ip4_payload_split(
	(uint8_t *)pkt, pktlen, &iph, NULL, NULL, NULL)) < 0) {
		errno = -ret;
		return ret;
	}

	struct sockaddr_in daddr = {
		.sin_family = AF_INET,
		/* Always 0 for raw socket */
		.sin_port = 0,
		.sin_addr = {
			.s_addr = iph->daddr
		}
	};

	pthread_mutex_lock(&rawsocket_lock);

	int sent = sendto(config.rawsocket, 
	    pkt, pktlen, 0, 
	    (struct sockaddr *)&daddr, sizeof(daddr));

	pthread_mutex_unlock(&rawsocket_lock);

	/* The function will return -errno on error as well as errno value set itself */
	if (sent < 0) sent = -errno;

	return sent;
}

struct packet_data {
	uint32_t id;
	uint16_t hw_proto;
	uint8_t hook;
	
	void *payload;
	uint16_t payload_len;
};

// Per-queue data. Passed to queue_cb.
struct queue_data {
	struct mnl_socket **_nl;
	int queue_num;
};

/**
 * Used to accept unsupported packets (GSOs)
 */
static int fallback_accept_packet(uint32_t id, struct queue_data qdata) {
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *verdnlh;
        verdnlh = nfq_nlmsg_put(buf, NFQNL_MSG_VERDICT, qdata.queue_num);
        nfq_nlmsg_verdict_put(verdnlh, id, NF_ACCEPT);

        if (mnl_socket_sendto(*qdata._nl, verdnlh, verdnlh->nlmsg_len) < 0) {
                perror("mnl_socket_send");
                return MNL_CB_ERROR;
        }

        return MNL_CB_OK;
}


struct dps_t {
	uint8_t *pkt;
	uint32_t pktlen;
	// Time for the packet in milliseconds
	uint32_t timer;
};
// Note that the thread will automatically release dps_t and pkt_buff
void *delay_packet_send(void *data) {
	struct dps_t *dpdt = data;

	uint8_t *pkt = dpdt->pkt;
	uint32_t pktlen = dpdt->pktlen;

	usleep(dpdt->timer * 1000);
	int ret = send_raw_socket(pkt, pktlen);
	if (ret < 0) {
		errno = -ret;
		perror("send delayed raw packet");
	}

	free(pkt);
	free(dpdt);
	return NULL;
}
		     
static int process_packet(const struct packet_data packet, struct queue_data qdata) {
	char buf[MNL_SOCKET_BUFFER_SIZE];
        struct nlmsghdr *verdnlh;

#ifdef DEBUG_LOGGING
        printf("packet received (id=%u hw=0x%04x hook=%u, payload len %u)\n",
	       packet.id, packet.hw_proto, packet.hook, packet.payload_len);
#endif

	if (packet.hw_proto != ETH_P_IP) {
		return fallback_accept_packet(packet.id, qdata);
	}

	const int family = AF_INET;

	const uint8_t *raw_payload = packet.payload;
	size_t raw_payload_len = packet.payload_len;

	const struct iphdr *iph;
	uint32_t iph_len;
	const struct tcphdr *tcph;
	uint32_t tcph_len;
	const uint8_t *data;
	uint32_t dlen;

	int ret = tcp4_payload_split((uint8_t *)raw_payload, raw_payload_len,
			      (struct iphdr **)&iph, &iph_len, (struct tcphdr **)&tcph, &tcph_len,
			      (uint8_t **)&data, &dlen);

	if (ret < 0) {
		goto fallback;
	}


	struct verdict vrd = analyze_tls_data(data, dlen);

	verdnlh = nfq_nlmsg_put(buf, NFQNL_MSG_VERDICT, qdata.queue_num);
        nfq_nlmsg_verdict_put(verdnlh, packet.id, NF_ACCEPT);

	if (vrd.target_sni) {
		if (config.verbose)
			printf("SNI target detected\n");

		if (dlen > 1480) {
			if (config.verbose)
				fprintf(stderr, "WARNING! Client Hello packet is too big and may cause issues!\n");
		}
		
		uint8_t frag1[MNL_SOCKET_BUFFER_SIZE];
		uint8_t frag2[MNL_SOCKET_BUFFER_SIZE];
		uint32_t f1len = MNL_SOCKET_BUFFER_SIZE;
		uint32_t f2len = MNL_SOCKET_BUFFER_SIZE;
		nfq_nlmsg_verdict_put(verdnlh, packet.id, NF_DROP);
		int ret = 0;

		nfq_ip_set_checksum((struct iphdr *)iph);
		nfq_tcp_compute_checksum_ipv4(
			(struct tcphdr *)tcph, (struct iphdr *)iph);

		if (config.fake_sni_strategy != FKSN_STRAT_NONE) {
			uint8_t fake_sni[MNL_SOCKET_BUFFER_SIZE];
			uint32_t fsn_len = MNL_SOCKET_BUFFER_SIZE;

			ret = gen_fake_sni(iph, tcph, fake_sni, &fsn_len);
			if (ret < 0) {
				errno = -ret;
				perror("gen_fake_sni");
				goto fallback;
			}

			ret = send_raw_socket(fake_sni, fsn_len);
			if (ret < 0) {
				errno = -ret;
				perror("send fake sni");
				goto fallback;
			}
		}

		size_t ipd_offset;
		size_t mid_offset;

		switch (config.fragmentation_strategy) {
			case FRAG_STRAT_TCP:
				ipd_offset = vrd.sni_offset;
				mid_offset = ipd_offset + vrd.sni_len / 2;

				if ((ret = tcp4_frag(raw_payload, raw_payload_len,
					mid_offset, frag1, &f1len, frag2, &f2len)) < 0) {

					errno = -ret;
					perror("tcp4_frag");
					goto fallback;
				}

				break;
			case FRAG_STRAT_IP:
				ipd_offset = ((char *)data - (char *)tcph) + vrd.sni_offset;
				mid_offset = ipd_offset + vrd.sni_len / 2;
				mid_offset += 8 - mid_offset % 8;

				if ((ret = ip4_frag(raw_payload, raw_payload_len,
					mid_offset, frag1, &f1len, frag2, &f2len)) < 0) {

					errno = -ret;
					perror("ip4_frag");
					goto fallback;
				}

				break;
			default:
				ret = send_raw_socket(raw_payload, raw_payload_len);
				if (ret < 0) {
					errno = -ret;
					perror("raw pack send");
				}
				goto fallback;
		}

		ret = send_raw_socket(frag2, f2len);
		if (ret < 0) {
			errno = -ret;
			perror("raw frags send: frag2");

			goto fallback;
		}

		if (config.seg2_delay) {
			struct dps_t *dpdt = malloc(sizeof(struct dps_t));
			dpdt->pkt = malloc(f1len);
			memcpy(dpdt->pkt, frag1, f1len);
			dpdt->pktlen = f1len;
			dpdt->timer = config.seg2_delay;
			pthread_t thr;
			pthread_create(&thr, NULL, delay_packet_send, dpdt);
			pthread_detach(thr);
		} else {
			ret = send_raw_socket(frag1, f1len);

			if (ret < 0) {
				errno = -ret;
				perror("raw frags send: frag1");

				goto fallback;
			}
		}
	}


/*       
	if (pktb_mangled(pktb)) {
		if (config.versose)
			printf("Mangled!\n");

		nfq_nlmsg_verdict_put_pkt(
			verdnlh, pktb_data(pktb), pktb_len(pktb));
	}
*/

send_verd:
        if (mnl_socket_sendto(*qdata._nl, verdnlh, verdnlh->nlmsg_len) < 0) {
                perror("mnl_socket_send");
		
		goto error;
        }
 
        return MNL_CB_OK;

fallback:
	return fallback_accept_packet(packet.id, qdata);
error:
	return MNL_CB_ERROR;
}

static int queue_cb(const struct nlmsghdr *nlh, void *data) {
	
	struct queue_data *qdata = data;

	struct nfqnl_msg_packet_hdr *ph = NULL;
        struct nlattr *attr[NFQA_MAX+1] = {0};
	struct packet_data packet = {0};

        if (nfq_nlmsg_parse(nlh, attr) < 0) {
                perror("Attr parse");
                return MNL_CB_ERROR;
        }
 
        if (attr[NFQA_PACKET_HDR] == NULL) {
		errno = ENODATA;
                perror("Metaheader not set");
                return MNL_CB_ERROR;
        }

        ph = mnl_attr_get_payload(attr[NFQA_PACKET_HDR]);

        packet.id = ntohl(ph->packet_id);
	packet.hw_proto = ntohs(ph->hw_protocol);
	packet.hook = ph->hook;
        packet.payload_len = mnl_attr_get_payload_len(attr[NFQA_PAYLOAD]);
        packet.payload = mnl_attr_get_payload(attr[NFQA_PAYLOAD]);

	if (attr[NFQA_CAP_LEN] != NULL && ntohl(mnl_attr_get_u32(attr[NFQA_CAP_LEN])) != packet.payload_len) {
		fprintf(stderr, "The packet was truncated! Skip!\n");
		return fallback_accept_packet(packet.id, *qdata);
	}

	if (attr[NFQA_MARK] != NULL) {
		// Skip packets sent by rawsocket to escape infinity loop.
		if ((ntohl(mnl_attr_get_u32(attr[NFQA_MARK])) & RAWSOCKET_MARK) == 
			RAWSOCKET_MARK) {
			return fallback_accept_packet(packet.id, *qdata);
		}
	}


	return process_packet(packet, *qdata);
}

#define BUF_SIZE (0xffff + (MNL_SOCKET_BUFFER_SIZE / 2))

int init_queue(int queue_num) {
	struct mnl_socket *nl;

	if (open_socket(&nl)) {
		perror("Unable to open socket");
		return -1;
	}

	uint32_t portid = mnl_socket_get_portid(nl);

	struct nlmsghdr *nlh;
	char buf[BUF_SIZE];

	nlh = nfq_nlmsg_put(buf, NFQNL_MSG_CONFIG, queue_num);
	nfq_nlmsg_cfg_put_cmd(nlh, AF_INET, NFQNL_CFG_CMD_BIND);

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		perror("mnl_socket_send");
		goto die;
	}

	nlh = nfq_nlmsg_put(buf, NFQNL_MSG_CONFIG, queue_num);
	nfq_nlmsg_cfg_put_params(nlh, NFQNL_COPY_PACKET, 0xffff);

	if (config.use_gso) {
		mnl_attr_put_u32(nlh, NFQA_CFG_FLAGS, htonl(NFQA_CFG_F_GSO));
		mnl_attr_put_u32(nlh, NFQA_CFG_MASK, htonl(NFQA_CFG_F_GSO));
	}

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		perror("mnl_socket_send");
		goto die;
	}


	/* ENOBUFS is signalled to userspace when packets were lost
          * on kernel side.  In most cases, userspace isn't interested
          * in this information, so turn it off.
          */
	int ret = 1;
	mnl_socket_setsockopt(nl, NETLINK_NO_ENOBUFS, &ret, sizeof(int));

	struct queue_data qdata = {
		._nl = &nl,
		.queue_num = queue_num
	};

	printf("Queue %d started!\n", qdata.queue_num);

	while (1) {
		ret = mnl_socket_recvfrom(nl, buf, BUF_SIZE);
		if (ret == -1) {
			perror("mnl_socket_recvfrom");
			continue;
		}

		ret = mnl_cb_run(buf, ret, 0, portid, queue_cb, &qdata);
		if (ret < 0) {
			perror("mnl_cb_run");
		}
	}


	close_socket(&nl);
	return 0;

die:
	close_socket(&nl);
	return -1;
}

// Per-queue config. Used to initialize a queue. Passed to wrapper
struct queue_conf {
	uint16_t i;
	int queue_num;
};

struct queue_res {
	int status;
};
static struct queue_res defqres = {0};

static struct queue_res threads_reses[MAX_THREADS];

void *init_queue_wrapper(void *qdconf) {
	struct queue_conf *qconf = qdconf;
	struct queue_res *thres = threads_reses + qconf->i;
	
	thres->status = init_queue(qconf->queue_num);

	fprintf(stderr, "Thread %d exited with status %d\n", qconf->i, thres->status);

	return thres;
}

int main(int argc, const char *argv[]) {
	if (parse_args(argc, argv)) {
		perror("Unable to parse args");
		exit(EXIT_FAILURE);
	}

	switch (config.fragmentation_strategy) {
		case FRAG_STRAT_TCP:
			printf("Using TCP segmentation\n");
			break;
		case FRAG_STRAT_IP:
			printf("Using IP fragmentation\n");
			break;
		default:
			printf("SNI fragmentation is disabled\n");
			break;
	}

	if (config.seg2_delay) {
		printf("Some outgoing googlevideo request segments will be delayed for %d ms as of seg2_delay define\n", config.seg2_delay);
	}

	switch (config.fake_sni_strategy) {
		case FKSN_STRAT_TTL:
			printf("Fake SNI will be sent before each googlevideo request, TTL strategy will be used with TTL %d\n", config.fake_sni_ttl);
			break;
		case FRAG_STRAT_IP:
			printf("Fake SNI will be sent before each googlevideo request, Ack-Seq strategy will be used\n");
			break;
		default:
			printf("SNI fragmentation is disabled\n");
			break;
	}

	if (config.use_gso) {
		printf("GSO is enabled\n");
	}

	if (open_raw_socket() < 0) {
		perror("Unable to open raw socket");
		exit(EXIT_FAILURE);
	}

	struct queue_res *qres = &defqres;

	if (config.threads == 1) {
		struct queue_conf tconf = {
			.i = 0,
			.queue_num = config.queue_start_num
		};

		qres = init_queue_wrapper(&tconf);
	} else {
		printf("%d threads wil be used\n", config.threads);

		struct queue_conf thread_confs[MAX_THREADS];
		pthread_t threads[MAX_THREADS];
		for (int i = 0; i < config.threads; i++) {
			struct queue_conf *tconf = thread_confs + i;
			pthread_t *thr = threads + i;

			tconf->queue_num = config.queue_start_num + i;
			tconf->i = i;

			pthread_create(thr, NULL, init_queue_wrapper, tconf);
		}

		void *res;
		for (int i = 0; i < config.threads; i++) {
			pthread_join(threads[i], &res);

			qres = res;
		}
	}

	if (close_raw_socket() < 0) {
		perror("Unable to close raw socket");
		exit(EXIT_FAILURE);
	}

	return qres->status;
}
