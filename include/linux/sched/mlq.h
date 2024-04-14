#ifndef _LINUX_SCHED_MLQ_H
#define _LINUX_SCHED_MLQ_H

#define MAX_MLQ_PRIO	4
#define MAX_MLQ_RR_PRIO	3

/* default timeslices is 100 msecs */
#define MLQ_TIMESLICE   (100 * HZ / 1000)

static inline int mlq_prio(int prio)
{
	if (unlikely(prio < MAX_MLQ_PRIO))
		return 1;
	return 0;
}

static inline int mlq_rr_get_timeslice(int prio)
{
	BUG_ON(prio <= 0 && prio >= MAX_MLQ_PRIO);
	return (prio >= MAX_MLQ_RR_PRIO)? 0 : MLQ_TIMESLICE / prio;
}

#endif /* _LINUX_SCHED_MLQ_H */
