#include <litmus/fdso.h>
#include <litmus/litmus.h>
#include <litmus/rt_domain.h>
#include <litmus/edf_common.h>

#include <linux/sched/signal.h>
#include <linux/sched/topology.h>
#include <linux/slab.h>

#include <litmus/sched_trace.h>
#include <litmus/trace.h>
#include <litmus/debug_trace.h>

#include <litmus/sched_gsnedf.h>
#include <litmus/gsnedf-omlp.h>

// From gsnedf
extern rt_domain_t gsnedf;

/* -=-=-=-=- OMLP support -=-=-=-=- */
long gsnedf_omlp_on_admit_task(struct task_struct * tsk) {
	tsk_rt(tsk)->omlp_heap_node = bheap_node_alloc(GFP_ATOMIC);
	if( !tsk_rt(tsk)->omlp_heap_node ) {
		printk(KERN_WARNING "GSN-EDF OMLP: could not allocate heap node for task %s/%d\n",
		       tsk->comm, tsk->pid);
		return -ENOMEM;
	}

	bheap_node_init(&tsk_rt(tsk)->omlp_heap_node, tsk);
	return 0;
}

void gsnedf_omlp_on_exit_task(struct task_struct * tsk) {
	BUG_ON(bheap_node_in_heap(tsk_rt(tsk)->omlp_heap_node));
	bheap_node_free(tsk_rt(tsk)->omlp_heap_node);
}

struct omlp_semaphore {
	struct litmus_lock litmus_lock;

	// current owner of lock
	struct task_struct *owner;

	// highest-priority waiter
	struct task_struct *hp_waiter;

	// FIFO queue of waiting tasks
	wait_queue_head_t fq;
	int fqsize;

	// Priority queue of waiting tasks
	struct bheap pq;
};

static inline struct omlp_semaphore* omlp_from_lock(struct litmus_lock* lock) {
	return container_of(lock, struct omlp_semaphore, litmus_lock);
}

// Caller is responsible for holding a lock to sem->fq.lock
// Function assumes "skip" is not the head of the pq, because other entries in the pq are not checked
struct task_struct* omlp_find_hp_waiter(struct omlp_semaphore *sem, struct task_struct* skip) {
	// To find the highest priority waiter, we iterate through the fq and compare against the head of the pq
	struct list_head *pos;
	struct task_struct *queued, *found = NULL;
	struct bheap_node* topnode;

	// First we check the FQ
	list_for_each(pos, &sem->fq.head) {
		queued = (struct task_struct*) list_entry( pos, wait_queue_entry_t, entry)->private;
		if( queued == skip ) continue;
		if( !edf_higher_prio(queued, found) ) continue;

		// Update the highest prio task found
		found = queued;
	}

	// Now we check the PQ
	topnode = bheap_peek(gsnedf.order, &sem->pq);
	if( topnode ) {
		struct task_struct *top_pq = bheap2task(topnode);
		if( top_pq != skip && edf_higher_prio(top_pq, found) )
			found = top_pq;
	}

	return found;
}

int gsnedf_omlp_lock(struct litmus_lock* l) {
	struct task_struct *t = current;
	struct omlp_semaphore *sem = omlp_from_lock(l);
	//wait_queue_entry_t wait;
	unsigned long flags;

	// Cannot do a lock op if we aren't a real-time task
	if (!is_realtime(t))
		return -EPERM;

	// Nested lock acquisition is not allowed because each task has a heap node, and
	// each node would need to be tracked per lock held. Currently, tasks only have one
	// heap node for the omlp, so nested acquisition is not possible, but doable in the future.
	if (tsk_rt(t)->num_locks_held)
		return -EBUSY;

	// Grab the FQ lock
	spin_lock_irqsave(&sem->fq.lock, flags);

	if( sem->owner ) {
		// Someone holds this resource

		/* FIXME: interruptible would be nice some day */
		set_current_state(TASK_UNINTERRUPTIBLE);

		if( sem->fqsize >= num_online_cpus() ) {
			// We need to go to the PQ
			bheap_insert(gsnedf.order, &sem->pq, tsk_rt(t)->omlp_heap_node);
		} else {
			// We go to the FQ
			init_waitqueue_entry(&tsk_rt(t)->omlp_fq_node, t);
			sem->fqsize++; // Record the number of FQ'd items, must be less than num cores
			__add_wait_queue_entry_tail_exclusive(&sem->fq, &tsk_rt(t)->omlp_fq_node);
		}

		if( edf_higher_prio(t, sem->hp_waiter) ) {
			sem->hp_waiter = t;
			if( edf_higher_prio(t, sem->owner) )
				gsnedf_set_priority_inheritance(sem->owner, sem->hp_waiter);
		}

		// Timestamp for suspending to wait on the lock
		TS_LOCK_SUSPEND;

		/* release lock before sleeping */
		spin_unlock_irqrestore(&sem->fq.lock, flags);

		// When schedule is called like this, we are deactivated, and only
		// the unlock function can wake us
		schedule();

		// Timestamp for resuming the lock
		TS_LOCK_RESUME;
	} else {
		// We hold the lock
		sem->owner = t;
		spin_unlock_irqrestore(&sem->fq.lock, flags);
	}

	// Update the number of locks held, and continue to critical-section
	tsk_rt(t)->num_locks_held++;
	
	return 0;
}

int gsnedf_omlp_unlock(struct litmus_lock* l) {
	struct task_struct *t = current, *nextfq, *pqtask;
	struct bheap_node *nextpq;
	struct omlp_semaphore *sem = omlp_from_lock(l);
	unsigned long flags;

	// Acquire the FIFO lock queue, we need to dequeue ourselves,
	// waken the next head (if any), and move one request from the PQ
	// to the FQ (if any)
	spin_lock_irqsave(&sem->fq.lock, flags);

	// Make sure the lock holder is the one unlocking
	if (sem->owner != t) {
		spin_unlock_irqrestore(&sem->fq.lock, flags);
		return -EINVAL;
	}

	// Reduce our locks held by 1
	tsk_rt(t)->num_locks_held--;

	// Check if there are jobs waiting for this resource.
	// Note, if the FQ is empty, then so is the PQ.
	nextfq = __waitqueue_remove_first(&sem->fq);
	if( !nextfq ) {
		// There are no more lock requests for this resource
		sem->owner = NULL;
	} else {
		// There is someone in the FQ.
		sem->owner = nextfq;

		TRACE_CUR("omlp lock ownership passed to %s/%d\n", nextfq->comm, nextfq->pid);

		// Now add the highest from the PQ to the FQ, if any.
		nextpq = bheap_take(gsnedf.order, &sem->pq);
		if( nextpq ) {
			// Insert this into the FQ tail
			pqtask = bheap2task(nextpq);
			init_waitqueue_entry(&tsk_rt(pqtask)->omlp_fq_node, pqtask );
			__add_wait_queue_entry_tail_exclusive(&sem->fq, &tsk_rt(pqtask)->omlp_fq_node);
		} else {
			// If there is nothing in the PQ, then we can decrease the FQ size
			sem->fqsize--;
		}

		// Determine the new highest priority waiter
		if( nextfq == sem->hp_waiter ) {
			TRACE_TASK(nextfq, "was highest-prio omlp waiter\n");
			sem->hp_waiter = omlp_find_hp_waiter(sem, nextfq);
			if (sem->hp_waiter)
				TRACE_TASK(sem->hp_waiter, "is new highest-prio omlp waiter\n");
			else
				TRACE("no further omlp waiters\n");
		} else {
			// If next is not the highest priority, then set up prio inheritance
			gsnedf_set_priority_inheritance(nextfq, sem->hp_waiter);
		}

		// wake up the new lock holder
		wake_up_process(nextfq);
	}

	// We need to remove our own prio inheritance (if any)
	if( tsk_rt(t)->inh_task )
		gsnedf_clear_priority_inheritance(t);
	spin_unlock_irqrestore(&sem->fq.lock, flags);
	return 0;
}

int gsnedf_omlp_close(struct litmus_lock* l) {
	struct task_struct *t = current;
	struct omlp_semaphore *sem = omlp_from_lock(l);
	unsigned long flags;

	// First, check to see if we own the lock, if so, we need to unlock
	int owner;
	spin_lock_irqsave(&sem->fq.lock,flags);
	owner = sem->owner == t;
	spin_unlock_irqrestore(&sem->fq.lock,flags);

	if( owner )
		gsnedf_omlp_unlock(l);
	return 0;
}

void gsnedf_omlp_free(struct litmus_lock* lock) {
	kfree(omlp_from_lock(lock));
}

static struct litmus_lock_ops gsnedf_omlp_lock_ops = {
	.close  = gsnedf_omlp_close,
	.lock   = gsnedf_omlp_lock,
	.unlock = gsnedf_omlp_unlock,
	.deallocate = gsnedf_omlp_free,
#ifdef CONFIG_LITMUS_LOCKING_WITHARGS
	.lock_arg = NULL,
#endif
};

struct litmus_lock* gsnedf_new_omlp(void) {
	struct omlp_semaphore* sem;

	// Allocate the semaphore
	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return NULL;

	// initialize lock parameters
	sem->owner = NULL;
	sem->hp_waiter = NULL;
	sem->fqsize = 0;
	init_waitqueue_head(&sem->fq);
	sem->litmus_lock.ops = &gsnedf_omlp_lock_ops;
	bheap_init(&sem->pq);

	return &sem->litmus_lock;
}