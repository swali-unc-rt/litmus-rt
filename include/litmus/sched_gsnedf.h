#ifndef _LITMUS_SCHED_GSNEDF_H
#define _LITMUS_SCHED_GSNEDF_H

#include <litmus/fdso.h>
#include <litmus/litmus.h>
#include <litmus/rt_domain.h>
#include <litmus/bheap.h>

/* cpu_entry_t - maintain the linked and scheduled state
 */
typedef struct  {
	int 			cpu;
	struct task_struct*	linked;		/* only RT tasks */
	struct task_struct*	scheduled;	/* only RT tasks */
	struct bheap_node*	hn;
} cpu_entry_t;

void gsnedf_check_for_preemptions(void);
int gsnedf_cpu_lower_prio(struct bheap_node *_a, struct bheap_node *_b);
noinline void gsnedf_job_arrival(struct task_struct* task);
noinline void gsnedf_unlink(struct task_struct* t);

#endif