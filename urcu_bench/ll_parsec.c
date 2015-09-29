#define __GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define __USE_GNU
#include <sched.h>
#include <pthread.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

#include <ck_brlock.h>
#include <ck_epoch.h>
#include <ck_pr.h>
#include <ck_spinlock.h>
#include <ck_rwlock.h>
#include <sys/mman.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "parsec.h"

#include <urcu.h>
#define N_OPS (10000000)

static char ops[N_OPS];

ck_rwlock_t rwlock = CK_RWLOCK_INITIALIZER;
ck_spinlock_mcs_t mcslock = CK_SPINLOCK_MCS_INITIALIZER;
;
// 0 -> ll, 1 -> parsec, 2-> urcu, 3->rwlock, 4->mcslock, 5->epoch, 6->brlock
#define BENCH_OP 6

struct mcs_context {
	ck_spinlock_mcs_context_t lock_context;
	char pad[PAGE_SIZE - sizeof(ck_spinlock_mcs_context_t)];
} __attribute__((packed, aligned(PAGE_SIZE)));

struct mcs_context mcslocks[NUM_CPU] __attribute__((aligned(PAGE_SIZE)));;

struct list_node {
	struct {
		ck_spinlock_mcs_t l;
		// avoid cache bouncing caused by prefetching! 
		char __padding[4*CACHE_LINE - sizeof(ck_spinlock_mcs_t)];
	} __attribute__((packed));
	char data[32];
	CK_SLIST_ENTRY(list_node) next;
} __attribute__((packed));

/********************************************************/
/** Following functions/data structures used for tests **/
/********************************************************/

static CK_SLIST_HEAD(test_list, list_node) ll_head = CK_SLIST_HEAD_INITIALIZER(ll_head);

void *
ll_add(void *arg)
{
	struct list_node *n;

	n = q_alloc(sizeof(struct list_node), 1);
	if (unlikely(!n)) {
		printf("Allocation error!\n");
		return NULL;
	}
	assert(!((unsigned long)n % CACHE_LINE));

	/* printf("q alloc n %p\n", n); */
	n->data[0] = (int)arg;
	CK_SLIST_INSERT_HEAD(&ll_head, n, next);

	return (void *)n;
}

void *
ll_remove(void *arg)
{
	int ret;
	struct list_node *n;
	(void)arg;

	n = CK_SLIST_FIRST(&ll_head);

	if (!n) return NULL;

	CK_SLIST_REMOVE_HEAD(&ll_head, next);
	ret = (int)(n->data[0]);
	q_free(n);

	return (void *)ret;
}

void
ll_init(void)
{
#if BENCH_OP != 0
	return;
#endif
	int i;

#define LL_LENGTH 100
#define NODES_PERTHD 2

	for (i = 0; i < (LL_LENGTH); i++) {
		ll_add((void *)((LL_LENGTH/NODES_PERTHD - 1) - i/NODES_PERTHD));
	}
	return;
}

void *
ll_traverse(void *arg)
{
	struct list_node *n;
	int i;

	(void)arg;

	i = 0;
	CK_SLIST_FOREACH(n, &ll_head, next) {
		i++;
	}

	return (void *)i;
}

struct mcs_locks {
	ck_spinlock_mcs_context_t l_context;
	char __padding[2*CACHE_LINE - sizeof(ck_spinlock_mcs_context_t)];
};

struct mcs_locks mcs_locks[NUM_CPU];
struct mcs_locks mcs_locks_2[NUM_CPU];

// pick the node to remove, and add a new one;
void *
ll_modify(void *arg)
{
	struct list_node *n, *new, *remove;
	ck_spinlock_mcs_context_t *mcs, *mcs2;
	int i, id, ret;
	int freed = 0;

	id = (int)arg;

	i = 0;
	new = q_alloc(sizeof(struct list_node), 1);
	assert(new);
	new->data[0] = id;
	mcs = &(mcs_locks[id].l_context);
	mcs2 = &(mcs_locks_2[id].l_context);

	CK_SLIST_FOREACH(n, &ll_head, next) {
		/* let's do modification to the list */
		if (i == id*NODES_PERTHD) {
			freed++;
			/* nodes owned by the current thd. */

			/* The simple lock sequence here assumes no
			 * contention -- if there is, the trylock
			 * could fail, and we should roll back in that
			 * case (because the current node might be
			 * freed already). */

			/* take current node lock + next node. */
			ret = ck_spinlock_mcs_trylock(&n->l, mcs);
			assert(ret);

			remove = CK_SLIST_NEXT(n, next);
			assert(remove->data[0] == id);

			ret = ck_spinlock_mcs_trylock(&remove->l, mcs2);
			assert(ret);

			assert(n && remove);
			/* replace remove with new */
			new->next.sle_next = remove->next.sle_next;
			ck_pr_fence_store();
			n->next.sle_next = new;
			/* release the old node. */
			q_free(remove);
			ck_spinlock_mcs_unlock(&remove->l, mcs2);
			ck_spinlock_mcs_unlock(&n->l, mcs);
		}
		i++;
	}
	assert(freed == 1);

	return (void *)i;
}

void *
nil_call(void *arg)
{
	(void)arg;

	return 0;
}

#define ITEM_SIZE (CACHE_LINE)

/* testing nil ops when not doing linked-list benchmark. */
unsigned int results[NUM_CPU][2];

#define N_LOG (N_OPS / NUM_CPU)
#define N_1P  (N_OPS / 100 / NUM_CPU)

unsigned long p99_log[N_LOG] CACHE_ALIGNED;

ck_epoch_t* global_epoch;
ck_brlock_t* brlock;

static int cmpfunc(const void * a, const void * b)
{
    return ( *(int*)a - *(int*)b );
}

void bench(void) {
	int i, id, ret = 0;
	unsigned long n_read = 0, n_update = 0, op_jump = NUM_CPU;
	unsigned long long s, e, s1, e1, tot_cost_r = 0, tot_cost_w = 0, max = 0, cost;
	void *last_alloc;

	ck_epoch_record_t* record = malloc(sizeof(struct ck_epoch_record));
	ck_epoch_register(global_epoch, record);

	ck_brlock_reader_t* reader = malloc(sizeof(struct ck_brlock_reader));
	ck_brlock_read_register(brlock, reader);
	
	(void)ret;
	id = get_cpu();
	last_alloc = q_alloc(ITEM_SIZE, 1);
	assert(last_alloc);

	s = get_time();
	for (i = 0 ; i < N_OPS/NUM_CPU; i++) {
//		printf("%d: %d\n", id, i);
		s1 = get_time();//tsc_start();//

		if (ops[(unsigned long)id+op_jump*i]) {
#if BENCH_OP == 0
			ret = (int)lib_exec(ll_modify, (void *)id);
#elif BENCH_OP == 1
			/* nil op -- alloc does quiescence */
			q_free(last_alloc);
			last_alloc = q_alloc(ITEM_SIZE , 1);
			assert(last_alloc);
#elif BENCH_OP == 3
			ck_rwlock_write_lock(&rwlock);
			ck_rwlock_write_unlock(&rwlock);
#elif BENCH_OP == 4
			ck_spinlock_mcs_lock(&mcslock, &(mcslocks[id].lock_context));
			ck_spinlock_mcs_unlock(&mcslock, &(mcslocks[id].lock_context));
#elif BENCH_OP == 2
			synchronize_rcu();
#elif BENCH_OP == 5
			ck_epoch_synchronize(global_epoch, record);
#elif BENCH_OP == 6
			ck_brlock_write_lock(brlock);
			ck_brlock_write_unlock(brlock);
#endif
			e1 = get_time();//tsc_start();//
			cost = e1-s1;
			tot_cost_w += cost;
			n_update++;

			if (id == 0) p99_log[N_LOG - n_update] = cost;
		} else {
#if BENCH_OP == 0
			ret = (int)lib_exec(ll_traverse, NULL);
			assert(ret == LL_LENGTH);
#elif BENCH_OP == 1
			ret = (int)lib_exec(nil_call, NULL);
#elif BENCH_OP == 3
			ck_rwlock_read_lock(&rwlock);
//			printf("%d readers\n", rwlock.n_readers);
			ck_rwlock_read_unlock(&rwlock);
#elif BENCH_OP == 4
			ck_spinlock_mcs_lock(&mcslock, &(mcslocks[id].lock_context));
			ck_spinlock_mcs_unlock(&mcslock, &(mcslocks[id].lock_context));
#elif BENCH_OP == 2
			rcu_read_lock();
			rcu_read_unlock();
#elif BENCH_OP == 5
			ck_epoch_begin(global_epoch, record);
			ck_epoch_end(global_epoch, record);
#elif BENCH_OP == 6
			ck_brlock_read_lock(brlock, reader);
			ck_brlock_read_unlock(reader);
#endif
			e1 = get_time();//tsc_start();//
			cost = e1-s1;
			tot_cost_r += cost;

			if (id == 0) p99_log[n_read] = cost;
			n_read++;
		}

		if (cost > max) max = cost;
	}
	assert(n_read + n_update <= N_LOG);
	e = get_time();

	if (n_read) tot_cost_r /= n_read;
	if (n_update) tot_cost_w /= n_update;

	results[id][0] = tot_cost_r;
	results[id][1] = tot_cost_w;

	if (id == 0) {
		unsigned long r_99 = 0, w_99 = 0;
		if (n_read) {
			qsort(p99_log, n_read, sizeof(unsigned long), cmpfunc);
			r_99 = p99_log[n_read - n_read / 100];
		}
		if (n_update) {
			qsort(&p99_log[n_read], n_update, sizeof(unsigned long), cmpfunc);
			w_99 = p99_log[N_LOG - 1 - n_update / 100];
		}
		printf("99p: read %lu write %lu\n", r_99, w_99);
	}


        printf("Thd %d: tot %lu ops (r %lu, u %lu) done, %llu (r %llu, w %llu) cycles per op, max %llu\n", 
               id, n_read+n_update, n_read, n_update, (unsigned long long)(e-s)/(n_read + n_update), 
               tot_cost_r, tot_cost_w, max);

	return;
}

void * 
worker(void *arg)
{
	quie_time_t s,e;
	int cpuid = (int)arg;

	thd_local_id = cpuid;
	thd_set_affinity(pthread_self(), thd_local_id);

	rcu_register_thread();
	rcu_init();
	

	s = get_time();
	/* printf("cpu %d (tid %d) starting @ %llu\n", thd_local_id, get_cpu(), s); */

	meas_sync_start();
	bench();
	meas_sync_end();

	if (cpuid == 0) {
		int i;
		unsigned long long tot_r = 0, tot_w = 0;
		for (i = 0; i < NUM_CPU; i++) {
			tot_r += results[i][0];
			tot_w += results[i][1];

			results[i][0] = 0;
			results[i][1] = 0;
		}
		tot_r /= NUM_CPU;
		tot_w /= NUM_CPU;

		printf("Summary: %s, (r %llu, w %llu) cycles per op\n", TRACE_FILE, tot_r, tot_w);
	}

	e = get_time();
	/* printf("cpu %d done (%llu to %llu)\n", cpuid, s, e); */

	return 0;
}

void load_trace()
{
	int fd, ret;
	int bytes;
	unsigned long i, n_read, n_update;
	char buf[PAGE_SIZE+1];

	ret = mlock(ops, N_OPS);
	if (ret) {
		printf("Cannot lock cache memory (%d). Check privilege. Exit.\n", ret);
		exit(-1);
	}

	printf("loading trace file @%s...\n", TRACE_FILE);
	/* read the entire trace into memory. */
	fd = open(TRACE_FILE, O_RDONLY);
	if (fd < 0) {
		printf("cannot open file %s. Exit.\n", TRACE_FILE);
		exit(-1);
	}
    
	for (i = 0; i < (N_OPS / PAGE_SIZE); i++) {
		bytes = read(fd, buf, PAGE_SIZE);
		assert(bytes == PAGE_SIZE);
		memcpy(&ops[i * PAGE_SIZE], buf, bytes);
	}

	if (N_OPS % PAGE_SIZE) {
		bytes = read(fd, buf, PAGE_SIZE);
		memcpy(&ops[i*PAGE_SIZE], buf, bytes);
	}
	n_read = n_update = 0;
	for (i = 0; i < N_OPS; i++) {
		if (ops[i] == 'R') { ops[i] = 0; n_read++; }
		else if (ops[i] == 'U') { ops[i] = 1; n_update++; }
		else assert(0);
	}
	printf("Trace: read %lu, update %lu, total %lu\n", n_read, n_update, (n_read+n_update));
	assert(n_read+n_update == N_OPS);

	close(fd);

	return;
}

extern volatile int use_ncores;

int main()
{
	int i, ret;
	pthread_t thds[NUM_CPU];

	ret = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (ret) {
		printf("cannot lock memory %d... exit.\n", ret);
		exit(-1);
	}

	global_epoch = malloc(sizeof(struct ck_epoch));
	ck_epoch_init(global_epoch);

	brlock = malloc(sizeof(struct ck_brlock));
	ck_brlock_init(brlock);

	rcu_init();

	parsec_init();

	ll_init();

	thd_set_affinity(pthread_self(), 0);

	load_trace();

#ifndef SYNC_USE_RDTSC
	create_timer_thd(NUM_CPU-1);
#endif
	for (i = 1; i < use_ncores; i++) {
		ret = pthread_create(&thds[i], 0, worker, (void *)i);
		if (ret) exit(-1);
	}

	usleep(50000);

	worker((void *)0);

	for (i = 1; i < use_ncores; i++) {
		pthread_join(thds[i], (void *)&ret);
	}

	return 0;
}
