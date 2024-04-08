#ifndef _LINUX_SCHED_MLQ_H
#define _LINUX_SCHED_MLQ_H

#include <linux/printk.h>

// valid prio from 1 to 3
#define MAX_MLQ_PRIO   3

#define MLQ_TIMESLICE   (100 * HZ / 1000)

// 50msec and 100msec timeslice
#define MLQ_TIMESLICE_50    (50 * HZ / 1000)
#define MLQ_TIMESLICE_100   (100 * HZ / 1000)

static inline int mlq_prio(int prio)
{
	pr_info("mlq_prio %d\n", prio);
	if (prio >= 1 && prio <= MAX_MLQ_PRIO)
		return 1;
	return 0;
}


#endif /* _LINUX_SCHED_MLQ_H */
