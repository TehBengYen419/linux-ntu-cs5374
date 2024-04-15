#include "sched.h"

void init_mlq_rq(struct mlq_rq *mlq_rq) {

	struct mlq_prio_array *array;
	int i;

	array = &mlq_rq->active;
	for (i = 0; i < MAX_MLQ_PRIO; i++) {
		INIT_LIST_HEAD(array->queue + i);
		__clear_bit(i, array->bitmap);
	}
	__set_bit(MAX_MLQ_PRIO, array->bitmap);
	mlq_rq->mlq_nr_running = 0;

#if defined CONFIG_SMP
	mlq_rq->highest_prio.curr = MAX_MLQ_PRIO - 1;
	mlq_rq->highest_prio.next = MAX_MLQ_PRIO - 1;
	plist_head_init(&mlq_rq->pushable_tasks);
#endif
}

static inline int on_mlq_rq(struct sched_mlq_entity *mlq_se)
{
	return mlq_se->on_rq;
}

static inline struct task_struct *mlq_task_of(struct sched_mlq_entity *mlq_se)
{
    return container_of(mlq_se, struct task_struct, mlq);
}

static inline int mlq_se_prio(struct sched_mlq_entity *mlq_se)
{
	/* 0: unused, 1,2: RR, 3: FIFO */
	return mlq_task_of(mlq_se)->prio;
}

static inline
int mlq_rr_nr_running(int prio)
{
	return (prio < MAX_MLQ_RR_PRIO)? 1 : 0;
}

#ifdef CONFIG_SMP
static void
inc_mlq_prio(struct mlq_rq *mlq_rq, int prio)
{
	int prev_prio = mlq_rq->highest_prio.curr;

	if (prio < prev_prio)
		mlq_rq->highest_prio.curr = prio;
}

static void
dec_mlq_prio(struct mlq_rq *mlq_rq, int prio)
{
	int prev_prio = mlq_rq->highest_prio.curr;

	if (mlq_rq->mlq_nr_running) {

		WARN_ON(prio < prev_prio);

		/*
		 * This may have been our highest task, and therefore
		 * we may have some recomputation to do
		 */
		if (prio == prev_prio) {
			struct mlq_prio_array *array = &mlq_rq->active;

			mlq_rq->highest_prio.curr =
				sched_find_first_bit(array->bitmap);
		}

	} else {
		mlq_rq->highest_prio.curr = MAX_MLQ_PRIO-1;
	}
}

static inline int has_pushable_tasks(struct rq *rq)
{
	return !plist_head_empty(&rq->mlq.pushable_tasks);
}

static void enqueue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->mlq.pushable_tasks);
	plist_node_init(&p->pushable_tasks, p->prio);
	plist_add(&p->pushable_tasks, &rq->mlq.pushable_tasks);

	if (p->prio < rq->mlq.highest_prio.next)
		rq->mlq.highest_prio.next = p->prio;
}

static void dequeue_pushable_task(struct rq *rq, struct task_struct *p)
{
	plist_del(&p->pushable_tasks, &rq->mlq.pushable_tasks);

	if (has_pushable_tasks(rq)) {
		p = plist_first_entry(&rq->mlq.pushable_tasks,
				      struct task_struct, pushable_tasks);
		rq->mlq.highest_prio.next = p->prio;
	} else {
		rq->mlq.highest_prio.next = MAX_MLQ_PRIO-1;
	}
}

static inline bool need_pull_mlq_task(struct rq *rq, struct task_struct *prev)
{
	/* if current prio belongs to mlq task or the sched_class with higher prio */
	return rq->online && (prev->prio < MAX_RT_PRIO); 
}

static int pick_mlq_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    cpumask_test_cpu(cpu, &p->cpus_mask))	// affinity
		return 1;

	return 0;
}

static struct task_struct *pick_highest_pushable_task(struct rq *rq, int cpu)
{
	struct plist_head *head = &rq->mlq.pushable_tasks;
	struct task_struct *p;

	if (!has_pushable_tasks(rq))
		return NULL;

	plist_for_each_entry(p, head, pushable_tasks) {
		if (pick_mlq_task(rq, p, cpu))
			return p;
	}

	return NULL;
}

#else	/* !CONFIG_SMP */
static inline void inc_mlq_prio(struct mlq_rq *mlq_rq, int prio) {}
static inline void dec_mlq_prio(struct mlq_rq *mlq_rq, int prio) {}
static inline void enqueue_pushable_task(struct rq *rq, struct task_struct *p) {}
static inline void dequeue_pushable_task(struct rq *rq, struct task_struct *p) {}
static void pull_mlq_task(struct rq *this_rq) {}
static inline bool need_pull_mlq_task(struct rq *rq, struct task_struct *prev)
{
	return false;
}
static struct task_struct *pick_highest_pushable_task(struct rq *rq, int cpu)
{
	return NULL;
}
#endif


static inline
void inc_mlq_tasks(struct sched_mlq_entity *mlq_se, struct mlq_rq *mlq_rq)
{
	int prio = mlq_se_prio(mlq_se);

	WARN_ON(!mlq_prio(prio));
	mlq_rq->mlq_nr_running++;
	mlq_rq->rr_nr_running += mlq_rr_nr_running(prio);
	
	inc_mlq_prio(mlq_rq, prio);
}

static inline
void dec_mlq_tasks(struct sched_mlq_entity *mlq_se, struct mlq_rq *mlq_rq)
{
	int prio = mlq_se_prio(mlq_se);

	WARN_ON(!mlq_prio(prio));
	WARN_ON(!mlq_rq->mlq_nr_running);
	mlq_rq->mlq_nr_running--;
	mlq_rq->rr_nr_running -= mlq_rr_nr_running(prio);

	dec_mlq_prio(mlq_rq, prio);
}

static void update_curr_mlq(struct rq *rq)
{
    struct task_struct *curr = rq->curr;
    u64 delta_exec;
	u64 now;

	if (curr->sched_class != &mlq_sched_class)
		return;

	now = rq_clock_task(rq);
    delta_exec = now - curr->se.exec_start;
    if (unlikely((s64)delta_exec <= 0))
		return;

    schedstat_set(curr->se.statistics.exec_max,
			max(curr->se.statistics.exec_max, delta_exec));

    curr->se.sum_exec_runtime += delta_exec;
    account_group_exec_runtime(curr, delta_exec);

    curr->se.exec_start = now;
    cgroup_account_cputime(curr, delta_exec);
}

static void enqueue_mlq_entity(struct rq *rq, struct sched_mlq_entity *mlq_se, int flags)
{
    struct mlq_prio_array *array = &rq->mlq.active;
	struct list_head *queue = array->queue + mlq_se_prio(mlq_se);

    if (flags & ENQUEUE_HEAD) {
        list_add(&mlq_se->task_list, queue);
	} else {
        list_add_tail(&mlq_se->task_list, queue);
	}

	__set_bit(mlq_se_prio(mlq_se), array->bitmap);
    mlq_se->on_rq = 1;

	inc_mlq_tasks(mlq_se, &rq->mlq);
}

static void dequeue_mlq_entity(struct rq *rq, struct sched_mlq_entity *mlq_se)
{
	struct mlq_prio_array *array = &rq->mlq.active;

    list_del_init(&mlq_se->task_list);
	if (list_empty(array->queue + mlq_se_prio(mlq_se)))
		__clear_bit(mlq_se_prio(mlq_se), array->bitmap);
    mlq_se->on_rq = 0;

	dec_mlq_tasks(mlq_se, &rq->mlq);
}

static void enqueue_task_mlq(struct rq *rq, struct task_struct *p, int flags)
{
    struct sched_mlq_entity *mlq_se = &p->mlq;

    if (flags & ENQUEUE_WAKEUP)
		mlq_se->timeout = 0;

    enqueue_mlq_entity(rq, mlq_se, flags);

	if (!task_current(rq, p) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
    add_nr_running(rq, 1);
}

static void dequeue_task_mlq(struct rq *rq, struct task_struct *p, int flags)
{
    struct sched_mlq_entity *mlq_se = &p->mlq;

    update_curr_mlq(rq);
    dequeue_mlq_entity(rq, mlq_se);

	dequeue_pushable_task(rq, p);
    sub_nr_running(rq, 1);
}

static void requeue_task_mlq(struct rq *rq, struct task_struct *p, int head)
{
	struct sched_mlq_entity *mlq_se = &p->mlq;

	if (on_mlq_rq(mlq_se))
	{
		struct mlq_prio_array *array = &rq->mlq.active;
		struct list_head *queue = array->queue + mlq_se_prio(mlq_se);

		if (head)
			list_move(&mlq_se->task_list, queue);
		else
			list_move_tail(&mlq_se->task_list, queue);
	}
}

static void yield_task_mlq(struct rq *rq)
{
	requeue_task_mlq(rq, rq->curr, 0);
}

static void check_preempt_curr_mlq(struct rq *rq, struct task_struct *p, int flags)
{
	if (p->sched_class == &mlq_sched_class
			&& rq->curr->sched_class == &mlq_sched_class
			&& p->prio < rq->curr->prio)
	{
		resched_curr(rq);
		return;
	}
	/* TODO: consider migration as same as rt if needed */
}

static inline void set_next_task_mlq(struct rq *rq, struct task_struct *p, bool first)
{
    p->se.exec_start = rq_clock_task(rq);

	dequeue_pushable_task(rq, p);
}

static struct sched_mlq_entity *pick_next_mlq_entity(struct rq *rq,
							struct mlq_rq *mlq_rq)
{
	struct mlq_prio_array *array = &mlq_rq->active;
	struct sched_mlq_entity *next = NULL;
	struct list_head *queue;
	int idx;

	idx = sched_find_first_bit(array->bitmap);
	BUG_ON(idx >= MAX_MLQ_PRIO);
	
	queue = array->queue + idx;
	next = list_entry(queue->next, struct sched_mlq_entity, task_list);

	return next;
}

static struct task_struct *_pick_next_task_mlq(struct rq *rq)
{
    struct sched_mlq_entity *mlq_se;
	struct mlq_rq *mlq_rq = &rq->mlq;

    mlq_se = pick_next_mlq_entity(rq, mlq_rq);
	BUG_ON(!mlq_se);
    
	return mlq_task_of(mlq_se);
}

static struct task_struct *pick_task_mlq(struct rq *rq)
{
    struct task_struct *p;

	if (!sched_mlq_runnable(rq))
		return NULL;

	p = _pick_next_task_mlq(rq);

	return p;
}

static struct task_struct *pick_next_task_mlq(struct rq *rq)
{
	struct task_struct *p = pick_task_mlq(rq);

	if (p)
    	set_next_task_mlq(rq, p, true);

	return p;
}

static void put_prev_task_mlq(struct rq *rq, struct task_struct *p)
{
    update_curr_mlq(rq);

	if (on_mlq_rq(&p->mlq) && p->nr_cpus_allowed > 1)
		enqueue_pushable_task(rq, p);
}

#ifdef CONFIG_POSIX_TIMERS
static void watchdog(struct rq *rq, struct task_struct *p)
{
	unsigned long soft, hard;

	/* max may change after cur was read, this will be fixed next tick */
	soft = task_rlimit(p, RLIMIT_RTTIME);
	hard = task_rlimit_max(p, RLIMIT_RTTIME);

	if (soft != RLIM_INFINITY) {
		unsigned long next;

		if (p->rt.watchdog_stamp != jiffies) {
			p->rt.timeout++;
			p->rt.watchdog_stamp = jiffies;
		}

		next = DIV_ROUND_UP(min(soft, hard), USEC_PER_SEC/HZ);
		if (p->rt.timeout > next) {
			posix_cputimers_rt_watchdog(&p->posix_cputimers,
						    p->se.sum_exec_runtime);
		}
	}
}
#else
static inline void watchdog(struct rq *rq, struct task_struct *p) { }
#endif

static void task_tick_mlq(struct rq *rq, struct task_struct *p, int queued)
{
    struct sched_mlq_entity *mlq_se = &p->mlq;
	int prio = mlq_se_prio(mlq_se);

    update_curr_mlq(rq);

	watchdog(rq, p);

    if (p->policy != SCHED_MLQ || !mlq_rr_nr_running(prio))
		return;

    if (--mlq_se->time_slice)
		return;

    mlq_se->time_slice = mlq_rr_get_timeslice(prio);
	WARN_ON(!mlq_se->time_slice);

    if (mlq_se->task_list.prev != mlq_se->task_list.next) {
        requeue_task_mlq(rq, p, 0);
        resched_curr(rq);
        return;
    }
}

static unsigned int get_rr_interval_mlq(struct rq *rq, struct task_struct *task)
{
	return mlq_rr_get_timeslice(task->prio);
}

static void prio_changed_mlq(struct rq *rq, struct task_struct *p, int oldprio) {}

static void switched_to_mlq(struct rq *rq, struct task_struct *p) {}

#ifdef CONFIG_SMP
static int
select_task_rq_mlq(struct task_struct *p, int cpu, int flags)
{
    struct rq *rq;
    int target = cpu, cpus, min;
    cpumask_t cpumask = p->cpus_mask;

    if (!(flags & (WF_TTWU | WF_FORK)))
		return target;

	rq = cpu_rq(cpu);
    min = rq->nr_running;

    rcu_read_lock();

    for_each_cpu(cpus, &cpumask) {
		
		if(cpus == cpu)
			continue;

        rq = cpu_rq(cpus);

        if ((rq->nr_running <= min) && cpu_online(cpus))
		{
			if (min == rq->nr_running &&
					p->prio > rq->mlq.highest_prio.curr)
				continue;

            min = rq->nr_running;
            target = cpus;
		} 
    }

    rcu_read_unlock();

    return target;
}

static void pull_mlq_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	bool resched = false;
	struct task_struct *p;
	struct rq *src_rq;
	int this_nr_running = this_rq->mlq.mlq_nr_running;

	for_each_online_cpu(cpu) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		if (src_rq->mlq.highest_prio.next >=
			this_rq->mlq.highest_prio.curr)
			continue;

		if (src_rq->mlq.mlq_nr_running > this_nr_running)
		{
        	double_lock_balance(this_rq, src_rq);

			p = pick_highest_pushable_task(src_rq, this_cpu);

			if (p && (p->prio < this_rq->mlq.highest_prio.curr)) {
				WARN_ON(p == src_rq->curr);
				WARN_ON(!task_on_rq_queued(p));

				if (p->prio < src_rq->curr->prio || is_migration_disabled(p))
					goto skip;

				deactivate_task(src_rq, p, 0);
				set_task_cpu(p, this_cpu);
				activate_task(this_rq, p, 0);

				/* 
				 * make increment to running tasks in this cpu,
				 * then we can continue to do migration on other tasks,
				 * if number of tasks in other cpu still larger than
				 * this cpu.
				 */
				this_nr_running++;
				resched = true;
			}
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}

	if (resched)
		resched_curr(this_rq);
}

static int balance_mlq(struct rq *rq, struct task_struct *p, struct rq_flags *rf) {

	if (!on_mlq_rq(&p->mlq) && need_pull_mlq_task(rq, p)) {
    	rq_unpin_lock(rq, rf);
		pull_mlq_task(rq);
		rq_repin_lock(rq, rf);
	}

	return sched_stop_runnable(rq) || sched_dl_runnable(rq)
		|| sched_rt_runnable(rq) || sched_mlq_runnable(rq);
}

static void rq_online_mlq(struct rq *rq) {}

static void rq_offline_mlq(struct rq *rq) {}

static void task_woken_mlq(struct rq *rq, struct task_struct *p) {}

static void switched_from_mlq(struct rq *rq, struct task_struct *p) {}

#endif /* CONFIG_SMP */

DEFINE_SCHED_CLASS(mlq) = {

    .enqueue_task	= enqueue_task_mlq,
    .dequeue_task	= dequeue_task_mlq,
    .yield_task		= yield_task_mlq,

    .check_preempt_curr = check_preempt_curr_mlq,

    .pick_next_task		= pick_next_task_mlq,
    .put_prev_task		= put_prev_task_mlq,
    .set_next_task		= set_next_task_mlq,

#ifdef CONFIG_SMP
    .balance			= balance_mlq,
	.pick_task			= pick_task_mlq,
    .select_task_rq		= select_task_rq_mlq,
    .set_cpus_allowed = set_cpus_allowed_common,
    .rq_online = rq_online_mlq,
    .rq_offline = rq_offline_mlq,
    .task_woken = task_woken_mlq,
    .switched_from = switched_from_mlq,
#endif

    .task_tick = task_tick_mlq,

    .get_rr_interval = get_rr_interval_mlq,

    .prio_changed = prio_changed_mlq,
    .switched_to = switched_to_mlq,

    .update_curr = update_curr_mlq,

#ifdef CONFIG_UCLAMP_TASK
    .uclamp_enabled = 1,
#endif
};
