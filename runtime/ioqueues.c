/*
 * ioqueues.c
 */

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <base/log.h>
#include <base/lrpc.h>
#include <base/mem.h>

#include <iokernel/shm.h>

#include <net/ethernet.h>
#include <net/mbuf.h>

#include "defs.h"

#define PACKET_QUEUE_MCOUNT 8192
#define COMMAND_QUEUE_MCOUNT 8192

DEFINE_SPINLOCK(qlock);
unsigned int nrqs = 0;

struct iokernel_control iok;
pthread_barrier_t barrier;

static int generate_random_mac(struct eth_addr *mac)
{
	int fd, ret;
	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -1;

	ret = read(fd, mac, sizeof(*mac));
	close(fd);
	if (ret != sizeof(*mac))
		return -1;

	mac->addr[0] &= ~ETH_ADDR_GROUP;
	mac->addr[0] |= ETH_ADDR_LOCAL_ADMIN;

	return 0;
}

// Could be a macro really, this is totally static :/
static size_t calculate_shm_space(unsigned int thread_count)
{

	size_t ret = 0, q;

	// Header + queue_spec information
	ret += sizeof(struct control_hdr);
	ret += sizeof(struct thread_spec) * thread_count;
	ret = align_up(ret, CACHE_LINE_SIZE);

	// Packet Queues
	q = sizeof(struct lrpc_msg) * PACKET_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += 2 * q * thread_count;

	q = sizeof(struct lrpc_msg) * COMMAND_QUEUE_MCOUNT;
	q = align_up(q, CACHE_LINE_SIZE);
	q += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);
	ret += q * thread_count;

	ret = align_up(ret, PGSIZE_2MB);

	// Egress buffers
	BUILD_ASSERT(ETH_MAX_LEN + sizeof(struct tx_net_hdr) <=
			MBUF_DEFAULT_LEN);
	BUILD_ASSERT(PGSIZE_2MB % MBUF_DEFAULT_LEN == 0);
	ret += MBUF_DEFAULT_LEN * PACKET_QUEUE_MCOUNT;
	ret = align_up(ret, PGSIZE_2MB);

	return ret;
}

static void ioqueue_alloc(struct shm_region *r, struct queue_spec *q,
			  char **ptr, size_t msg_count)
{
	q->msg_buf = ptr_to_shmptr(r, *ptr, sizeof(struct lrpc_msg) * msg_count);
	*ptr += align_up(sizeof(struct lrpc_msg) * msg_count, CACHE_LINE_SIZE);

	q->wb = ptr_to_shmptr(r, *ptr, sizeof(uint32_t));
	*ptr += align_up(sizeof(uint32_t), CACHE_LINE_SIZE);

	q->msg_count = msg_count;
}

static int ioqueues_shm_setup(unsigned int threads)
{
	struct shm_region *r = &netcfg.tx_region, *ingress_region = &netcfg.rx_region;
	char *ptr;
	int i, ret;
	size_t shm_len;

	ret = generate_random_mac(&netcfg.mac);
	if (ret < 0)
		return ret;

	BUILD_ASSERT(sizeof(netcfg.mac) >= sizeof(mem_key_t));
	iok.key = *(mem_key_t*)(&netcfg.mac);

	/* map shared memory for control header, command queues, and egress pkts */
	shm_len = calculate_shm_space(threads);
	r->len = shm_len;
	r->base = mem_map_shm(iok.key, NULL, shm_len, PGSIZE_2MB, true);
	if (r->base == MAP_FAILED) {
		log_err("control_setup: mem_map_shm() failed");
		return -1;
	}

	/* map ingress memory */
	ingress_region->base =
	    mem_map_shm(INGRESS_MBUF_SHM_KEY, NULL, INGRESS_MBUF_SHM_SIZE,
			PGSIZE_2MB, false);
	if (ingress_region->base == MAP_FAILED) {
		log_err("control_setup: failed to map ingress region");
		mem_unmap_shm(r->base);
		return -1;
	}
	ingress_region->len = INGRESS_MBUF_SHM_SIZE;

	/* set up queues in shared memory */
	iok.thread_count = threads;
	ptr = r->base;
	ptr += sizeof(struct control_hdr) + sizeof(struct thread_spec) * threads;
	ptr = (char *)align_up((uintptr_t)ptr, CACHE_LINE_SIZE);

	for (i = 0; i < threads; i++) {
		struct thread_spec *tspec = &iok.threads[i];
		ioqueue_alloc(r, &tspec->rxq, &ptr, PACKET_QUEUE_MCOUNT);
		ioqueue_alloc(r, &tspec->txpktq, &ptr, PACKET_QUEUE_MCOUNT);
		ioqueue_alloc(r, &tspec->txcmdq, &ptr, COMMAND_QUEUE_MCOUNT);
	}

	ptr = (char *)align_up((uintptr_t)ptr, PGSIZE_2MB);
	iok.tx_buf = ptr;
	iok.tx_len = MBUF_DEFAULT_LEN * PACKET_QUEUE_MCOUNT;

	ptr_to_shmptr(r, ptr, iok.tx_len);
	ptr += iok.tx_len;

	iok.next_free = ptr_to_shmptr(r, ptr, 0);

	return 0;
}

static void ioqueues_shm_cleanup(void)
{
	mem_unmap_shm(netcfg.tx_region.base);
	mem_unmap_shm(netcfg.rx_region.base);
}

/*
 * Register this runtime with the IOKernel. All threads must complete their
 * per-thread ioqueues initialization before this function is called.
 */
int ioqueues_register_iokernel(void)
{
	struct control_hdr *hdr;
	struct shm_region *r = &netcfg.tx_region;
	struct sockaddr_un addr;
	int ret;

	/* initialize control header */
	hdr = r->base;
	hdr->magic = CONTROL_HDR_MAGIC;
	hdr->thread_count = iok.thread_count;
	hdr->mac = netcfg.mac;

	hdr->sched_cfg.priority = SCHED_PRIORITY_NORMAL;
	hdr->sched_cfg.max_cores = iok.thread_count;
	hdr->sched_cfg.congestion_latency_us = 0;
	hdr->sched_cfg.scaleout_latency_us = 0;

	memcpy(hdr->threads, iok.threads,
			sizeof(struct thread_spec) * iok.thread_count);

	/* register with iokernel */
	BUILD_ASSERT(strlen(CONTROL_SOCK_PATH) <= sizeof(addr.sun_path) - 1);
	memset(&addr, 0x0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);

	iok.fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (iok.fd == -1) {
		log_err("register_iokernel: socket() failed [%s]", strerror(errno));
		goto fail;
	}

	if (connect(iok.fd, (struct sockaddr *)&addr,
		 sizeof(struct sockaddr_un)) == -1) {
		log_err("register_iokernel: connect() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	ret = write(iok.fd, &iok.key, sizeof(iok.key));
	if (ret != sizeof(iok.key)) {
		log_err("register_iokernel: write() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	ret = write(iok.fd, &netcfg.tx_region.len, sizeof(netcfg.tx_region.len));
	if (ret != sizeof(netcfg.tx_region.len)) {
		log_err("register_iokernel: write() failed [%s]", strerror(errno));
		goto fail_close_fd;
	}

	return 0;

fail_close_fd:
	close(iok.fd);
fail:
	ioqueues_shm_cleanup();
	return -errno;

}

int ioqueues_init_thread(void)
{
	int ret;
	struct shm_region *r = &netcfg.tx_region;

	spin_lock(&qlock);
	assert(nrqs < iok.thread_count);
	struct thread_spec *ts = &iok.threads[nrqs++];
	spin_unlock(&qlock);

	ret = shm_init_lrpc_in(r, &ts->rxq, &myk()->rxq);
	BUG_ON(ret);

	ret = shm_init_lrpc_out(r, &ts->txpktq, &myk()->txpktq);
	BUG_ON(ret);

	ret = shm_init_lrpc_out(r, &ts->txcmdq, &myk()->txcmdq);
	BUG_ON(ret);

	pthread_barrier_wait(&barrier);

	return 0;
}

/*
 * General initialization for runtime <-> iokernel communication. Must be
 * called before per-thread ioqueues initialization.
 */
int ioqueues_init(unsigned int threads)
{
	int ret;

	spin_lock_init(&qlock);
	pthread_barrier_init(&barrier, NULL, maxks);

	ret = ioqueues_shm_setup(threads);
	if (ret) {
		log_err("ioqueues_init: ioqueues_shm_setup() failed, ret = %d", ret);
		return ret;
	}

	return 0;
}
