#ifndef _LINUX_SCHED_MLQ_H
#define _LINUX_SCHED_MLQ_H

#define MLQ_MAX_PRIORITY   3

#define MLQ_TIMESLICE   (100 * HZ / 1000)

// 50msec and 100msec timeslice
#define MLQ_TIMESLICE_50    (50 * HZ / 1000)
#define MLQ_TIMESLICE_100   (100 * HZ / 1000)

#endif /* _LINUX_SCHED_MLQ_H */
