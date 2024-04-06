#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/sched/mlq.h>
#include <linux/sched/task.h>

#include "sched.h"

void init_mlq_rq(struct mlq_rq *mlq_rq) {
    INIT_LIST_HEAD(&mlq_rq->task_list);
    mlq_rq->mlq_nr_running = 0;
}

static inline int on_mlq_rq(struct sched_mlq_entity *mlq_se) { return mlq_se->on_rq; }

static inline struct task_struct *mlq_task_of(struct sched_mlq_entity *mlq_se) {
    return container_of(mlq_se, struct task_struct, mlq);
}

static void enqueue_mlq_entity(struct rq *rq, struct sched_mlq_entity *mlq_se, bool head) {
    struct list_head *queue = &rq->mlq.task_list;

    if (head)
        list_add(&mlq_se->task_list, queue);
    else
        list_add_tail(&mlq_se->task_list, queue);

    mlq_se->on_rq = 1;

    ++rq->mlq.mlq_nr_running;
}

static void dequeue_mlq_entity(struct rq *rq, struct sched_mlq_entity *mlq_se) {
    list_del_init(&mlq_se->task_list);
    mlq_se->on_rq = 0;
    --rq->mlq.mlq_nr_running;
}

static void enqueue_task_mlq(struct rq *rq, struct task_struct *p, int flags) {
    struct sched_mlq_entity *mlq_se = &p->mlq;

    if (flags & ENQUEUE_WAKEUP) mlq_se->timeout = 0;

    enqueue_mlq_entity(rq, mlq_se, flags & ENQUEUE_HEAD);
    add_nr_running(rq, 1);
}

static void update_curr_mlq(struct rq *rq) {
    struct task_struct *curr = rq->curr;
    u64 delta_exec;

    delta_exec = rq_clock_task(rq) - curr->se.exec_start;
    if (unlikely((s64)delta_exec < 0)) delta_exec = 0;

    schedstat_set(curr->se.statistics.exec_max, max(curr->se.statistics.exec_max, delta_exec));

    curr->se.sum_exec_runtime += delta_exec;
    account_group_exec_runtime(curr, delta_exec);

    curr->se.exec_start = rq_clock_task(rq);
    cgroup_account_cputime(curr, delta_exec);
}

static void dequeue_task_mlq(struct rq *rq, struct task_struct *p, int flags) {
    struct sched_mlq_entity *mlq_se = &p->mlq;

    update_curr_mlq(rq);
    dequeue_mlq_entity(rq, mlq_se);
    sub_nr_running(rq, 1);
}

static void requeue_task_mlq(struct rq *rq, struct task_struct *p) {
    list_move_tail(&p->mlq.task_list, &rq->mlq.task_list);
}

static void yield_task_mlq(struct rq *rq) { requeue_task_mlq(rq, rq->curr); }

/* No preemption */
static void check_preempt_curr_mlq(struct rq *rq, struct task_struct *p, int flags) {}

static inline void set_next_task_mlq(struct rq *rq, struct task_struct *p, bool first) {
    p->se.exec_start = rq_clock_task(rq);
}

static struct task_struct *pick_next_task_mlq(struct rq *rq) {
    struct task_struct *next;
    struct sched_mlq_entity *next_se;

    if (!rq->mlq.mlq_nr_running) return NULL;

    next_se = list_first_entry(&rq->mlq.task_list, struct sched_mlq_entity, task_list);
    next = mlq_task_of(next_se);
    if (!next) return NULL;

    next->se.exec_start = rq_clock_task(rq);
    set_next_task_mlq(rq, next, true);
    return next;
}

static void task_tick_mlq(struct rq *rq, struct task_struct *p, int queued) {
    struct sched_mlq_entity *mlq_se = &p->mlq;

    update_curr_mlq(rq);

    if (p->policy != SCHED_MLQ) return;

    if (--mlq_se->time_slice) return;

    mlq_se->time_slice = MLQ_TIMESLICE;

    if (mlq_se->task_list.prev != mlq_se->task_list.next) {
        requeue_task_mlq(rq, p);
        resched_curr(rq);
        return;
    }
}

/* No preemption so no priority */
static void prio_changed_mlq(struct rq *rq, struct task_struct *p, int oldprio) {}

static void switched_to_mlq(struct rq *rq, struct task_struct *p) {}

static unsigned int get_rr_interval_mlq(struct rq *rq, struct task_struct *task) { return MLQ_TIMESLICE; }

static void put_prev_task_mlq(struct rq *rq, struct task_struct *p) {
    if (on_mlq_rq(&p->mlq)) update_curr_mlq(rq);
}

#ifdef CONFIG_SMP

static int can_migrate_task(struct task_struct *p, struct rq *src_rq, struct rq *dst_rq) {
    // p's policy has to be MY_RR
    if (p->policy != SCHED_MLQ) return 0;
    // if cpu is offline then don't move
    if (!cpu_active(dst_rq->cpu)) return 0;
    // Do not steal a task from CPUs with fewer than 2 tasks.
    if (src_rq->mlq.mlq_nr_running < 2) return 0;
    // Make sure to respect the CPU affinity of a given task.
    if (!cpumask_test_cpu(dst_rq->cpu, p->cpus_ptr)) return 0;
    // Do not move tasks that are currently running on a CPU (obviously).
    if (task_running(src_rq, p)) return 0;
    // Make sure to respect per-CPU kthreads. These should remain on their specified CPUs.
    if (kthread_is_per_cpu(p)) return 0;
    // if not in current cpu then don't move
    if (task_cpu(p) != src_rq->cpu) return 0;

    return 1;
}

static int select_task_rq_mlq(struct task_struct *p, int cpu, int sd_flag) {
    struct rq *rq;
    int cpus;
    int min;
    int best_cpu;
    cpumask_t cpumask = p->cpus_mask;

    if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK) return cpu;

    rcu_read_lock();

    min = -1;

    for_each_cpu(cpus, &cpumask) {
        rq = cpu_rq(cpus);

        if ((min == -1 || min > rq->nr_running) && cpu_online(cpus)) {
            min = rq->nr_running;
            best_cpu = cpus;
        }
    }

    rcu_read_unlock();

    if (min == -1) return cpu;

    return best_cpu;
}

static struct task_struct *pick_loadable_task(struct rq *rq, struct rq *dst_rq) {
    struct list_head *head = &rq->mlq.task_list;
    struct task_struct *p;
    struct sched_mlq_entity *se;

    // Do not steal a task from CPUs with fewer than 2 tasks.
    if (rq->mlq.mlq_nr_running < 2) return NULL;

    se = list_last_entry(&rq->mlq.task_list, struct sched_mlq_entity, task_list);
    p = mlq_task_of(se);

    list_for_each_entry(se, head, task_list) {
        p = mlq_task_of(se);
        if (can_migrate_task(p, rq, dst_rq)) return p;
    }

    return NULL;
}

static int balance_mlq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf) {
    int max_nr_running = 0;
    int this_cpu = rq->cpu, cpu;
    struct task_struct *p;
    struct rq *src_rq, *busiest_rq;

    rq_unpin_lock(rq, rf);

    if (rq->nr_running != 0) {
        rq_repin_lock(rq, rf);
        return 0;
    }

    for_each_online_cpu(cpu) {
        if (this_cpu == cpu) continue;

        src_rq = cpu_rq(cpu);

        if (max_nr_running >= src_rq->mlq.mlq_nr_running) continue;

        max_nr_running = src_rq->mlq.mlq_nr_running;
        busiest_rq = cpu_rq(cpu);
    }

    if (max_nr_running != 0) {
        double_lock_balance(rq, busiest_rq);

        p = pick_loadable_task(busiest_rq, rq);

        if (!p) {
            double_unlock_balance(rq, busiest_rq);
            goto out;
        }

        deactivate_task(busiest_rq, p, 0);
        set_task_cpu(p, this_cpu);
        activate_task(rq, p, 0);

        double_unlock_balance(rq, busiest_rq);

        resched_curr(rq);

        rq_repin_lock(rq, rf);
        return 1;
    }

out:
    rq_repin_lock(rq, rf);

    return 0;
}

static void rq_online_mlq(struct rq *rq) {}

static void rq_offline_mlq(struct rq *rq) {}

static void task_woken_mlq(struct rq *rq, struct task_struct *p) {}

static void switched_from_mlq(struct rq *rq, struct task_struct *p) {}

#endif /* CONFIG_SMP */

const struct sched_class mlq_sched_class __section("__mlq_sched_class") = {
    .enqueue_task = enqueue_task_mlq,
    .dequeue_task = dequeue_task_mlq,
    .yield_task = yield_task_mlq,

    .check_preempt_curr = check_preempt_curr_mlq,

    .pick_next_task = pick_next_task_mlq,
    .put_prev_task = put_prev_task_mlq,
    .set_next_task = set_next_task_mlq,

#ifdef CONFIG_SMP
    .balance = balance_mlq,
    .select_task_rq = select_task_rq_mlq,

    .rq_online = rq_online_mlq,
    .rq_offline = rq_offline_mlq,
    .task_woken = task_woken_mlq,
    .set_cpus_allowed = set_cpus_allowed_common,

    .switched_from = switched_from_mlq,
#endif

    .task_tick = task_tick_mlq,

    .switched_to = switched_to_mlq,
    .prio_changed = prio_changed_mlq,
    .get_rr_interval = get_rr_interval_mlq,

    .update_curr = update_curr_mlq,
#ifdef CONFIG_UCLAMP_TASK
    .uclamp_enabled = 1,
#endif
};