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
#include <litmus/gsnedf-smlp.h>

// From gsnedf
extern rt_domain_t gsnedf;
#define gsnedf_lock (gsnedf.ready_lock)

/* -=-=-=-=- SMLP support -=-=-=-=- */
long gsnedf_smlp_on_admit_task(struct task_struct * tsk) {
	tsk_rt(tsk)->smlp_pq_node = bheap_node_alloc(GFP_ATOMIC);
	if( !tsk_rt(tsk)->smlp_pq_node ) {
		printk(KERN_WARNING "GSN-EDF OMLP: could not allocate heap node for task %s/%d\n",
		       tsk->comm, tsk->pid);
		return -ENOMEM;
	}

	bheap_node_init(&tsk_rt(tsk)->smlp_pq_node, tsk);
    tsk_rt(tsk)->smlp_assigned_mask = 0;
	tsk_rt(tsk)->smlp_lock_arg = NULL;
	tsk_rt(tsk)->smlp_piq_node.entry.next = NULL;
	tsk_rt(tsk)->smlp_piq_node.entry.prev = NULL;
	return 0;
}

void gsnedf_smlp_on_exit_task(struct task_struct * tsk) {
	BUG_ON(bheap_node_in_heap(tsk_rt(tsk)->smlp_pq_node));
	bheap_node_free(tsk_rt(tsk)->smlp_pq_node);
}

struct smlp_semaphore {
	struct litmus_lock litmus_lock;

	// highest-priority waiter
	struct task_struct *hp_waiter;

    // prio-inheritance queue- only one job can inherit prio at a time
    wait_queue_head_t piq;

    // Satisfied queue of GPU running tasks
    wait_queue_head_t sq;

	// FIFO queue of waiting tasks
	wait_queue_head_t fq;
	int fqsize;

	// Priority queue of waiting tasks
	struct bheap pq;

    // Mask to assign
    // If bit n is set, then TPC n is up for grabs
    uint64_t mask;

	// If this is set, then LRT_smlp_gpu_done needs to be called before moving to the PIQ
	int explicit_gpu_finish;
};

static inline struct smlp_semaphore* smlp_from_lock(struct litmus_lock* lock) {
	return container_of(lock, struct smlp_semaphore, litmus_lock);
}

int __gsnedf_smlp_on_gpu_done(struct litmus_lock *l) {
	struct smlp_semaphore *sem;
	struct task_struct *firstpiq, *tsk = current;
	unsigned long flags;

	// Cannot do a lock op if we aren't a real-time task
	if (!is_realtime(tsk))
		return -EPERM;

	sem = smlp_from_lock( l );

	// Grab the FQ lock
	spin_lock_irqsave(&sem->fq.lock, flags);

	// Remove ourselves from the SQ
	list_del(&tsk_rt(tsk)->smlp_sq_node.entry);

	// Add ourselves to the PIQ so we can finalize via unlock.
	// Check first to make sure we're not already in there (can happen
	// if suspended multiple times before completing)
	if( tsk_rt(tsk)->smlp_piq_node.entry.next == NULL &&
	    tsk_rt(tsk)->smlp_piq_node.entry.prev == NULL ) {
		init_waitqueue_entry(&tsk_rt(tsk)->smlp_piq_node, tsk);
		__add_wait_queue_entry_tail_exclusive(&sem->piq, &tsk_rt(tsk)->smlp_piq_node);

		// Are we the head request? if so, set up priority inheritance
		firstpiq = __waitqueue_peek_first(&sem->piq);
		if( sem->hp_waiter != tsk && firstpiq == tsk && edf_higher_prio(sem->hp_waiter, tsk) ) {
			gsnedf_set_priority_inheritance_nogsnedflock(tsk, sem->hp_waiter);
		}
	}

	spin_unlock_irqrestore(&sem->fq.lock, flags);
	return 0;
}

int gsnedf_smlp_on_gpu_done(struct litmus_lock *l) {
	struct task_struct* tsk = current;
	struct smlp_semaphore *sem;
	int rv;

	// Cannot do a lock op if we aren't a real-time task
	if (!is_realtime(tsk))
		return -EPERM;

	sem = smlp_from_lock(l);

	// User should only be calling this if explicit marking the gpu as done was specified
	if( sem->explicit_gpu_finish ) {
		raw_spin_lock(&gsnedf_lock);
		rv = __gsnedf_smlp_on_gpu_done(&sem->litmus_lock);
		raw_spin_unlock(&gsnedf_lock);
		return rv;
	}

	return -EINVAL;
}

// If a suspended SMLP task re-arrives, then it is put in the PIQ
// and allowed to call its unlock function. Can assume the GPU segment is over.
void gsnedf_smlp_on_task_arrival(struct task_struct * tsk) {
	struct smlp_semaphore *sem;

	// Cannot do a lock op if we aren't a real-time task
	if (!is_realtime(tsk))
		return;

	// Can we not retrieve the litmus_lock? (task might not be related to SMLP)
	if( !tsk_rt(tsk)->smlp_lock_arg )
		return;

	sem = smlp_from_lock( (struct litmus_lock*) tsk_rt(tsk)->smlp_lock_arg );

	// If we don't need an explicit finish, we can assume task arrivals means move to PIQ
	if( !sem->explicit_gpu_finish ) {
		__gsnedf_smlp_on_gpu_done(&sem->litmus_lock);
	}
}

// Caller is responsible for holding a lock to sem->fq.lock
// Function assumes "skip" is not the head of the pq, because other entries in the pq are not checked
struct task_struct* smlp_find_hp_waiter(struct smlp_semaphore *sem, struct task_struct* skip) {
	// To find the highest priority waiter, we iterate through the fq and compare against the head of the pq
    // For the SMLP, we also need to check the SQ
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
		if( top_pq != skip ) {
			if( edf_higher_prio(top_pq, found))
				found = top_pq;
		} else {
			// Check second highest in the PQ here if top_pq is skip
			// struct task_struct *second_pq = NULL;
			// if( topnode->next ) {
			// 	second_pq = bheap2task(topnode->next);
			// 	if( second_pq != skip && edf_higher_prio(second_pq, found) )
			// 		found = second_pq;
			// }
		}
	}

    // Lastly check the SQ
    list_for_each(pos, &sem->sq.head) {
		queued = (struct task_struct*) list_entry( pos, wait_queue_entry_t, entry)->private;
		if( queued == skip ) continue;
		if( !edf_higher_prio(queued, found) ) continue;

		// Update the highest prio task found
		found = queued;
	}

	// We also need to check the PIQ
	list_for_each(pos, &sem->piq.head) {
		queued = (struct task_struct*) list_entry( pos, wait_queue_entry_t, entry)->private;
		if( queued == skip ) continue;
		if( !edf_higher_prio(queued, found) ) continue;

		// Update the highest prio task found
		found = queued;
	}

	return found;
}

// TODO: fix this later, __builtin_popcountll not linking
int popcountll(uint64_t x) {
	int count = 0;
	while( x ) {
		count += x & 0x1;
		x >>= 1;
	}
	return count;
}

static uint64_t get_smlp_mask(uint64_t current_mask, uint64_t allowed_tpc_bits) {
	// assign mask to this request, first, how many SMs are available?
	int tpcsToUse = popcountll(current_mask);
	uint64_t assigned_mask = 0;
	int i, tpc;

	// Can this request take this many TPCs?
	while( !(allowed_tpc_bits & (1ULL << (tpcsToUse-1))) )
		tpcsToUse--;

	// Do the assignment of TPCs
	assigned_mask = 0;
	for( i = 0; i < tpcsToUse; ++i ) {
		tpc = __builtin_ffsll(current_mask) - 1;
		current_mask &= ~(1ULL << tpc);
		assigned_mask |= (1ULL << tpc);
	}

	//lock_arg->assigned_mask = tsk_rt(t)->smlp_assigned_mask;
	return assigned_mask;
}

static int __gsnedf_smlp_lock(struct litmus_lock* l, struct smlp_lock_arg* lock_arg) {
	struct task_struct *t = current, *firstpiq;
	struct smlp_semaphore *sem = smlp_from_lock(l);
	unsigned long flags;

	// Cannot do a lock op if we aren't a real-time task
	if (!is_realtime(t))
		return -EPERM;

	// Nested lock acquisition is not allowed because each task has a heap node, and
	// each node would need to be tracked per lock held. Currently, tasks only have one
	// heap node for the omlp, so nested acquisition is not possible, but doable in the future.
	if (tsk_rt(t)->num_locks_held)
		return -EBUSY;

	// Note, the first bit has to be set. The SMLP requires being able to lock at least one TPC.
	if( !(lock_arg->allowed_tpc_bits & 0x1) )
		return -EINVAL;

	// Grab the FQ lock
	spin_lock_irqsave(&sem->fq.lock, flags);

	// Update the highest-prio person waiting
	if( edf_higher_prio(t, sem->hp_waiter) ) {
		sem->hp_waiter = t;
		if ( waitqueue_active(&sem->piq) ) {
			// PIQ is active, so set up prio inheritance
			firstpiq = __waitqueue_peek_first(&sem->piq);
			gsnedf_set_priority_inheritance( firstpiq, sem->hp_waiter );
		}
	}

	// We can either fit in the FQ or SQ
	if( !sem->mask ) {
		// No available TPCs, go to PQ->FQ
		set_current_state(TASK_UNINTERRUPTIBLE);

		tsk_rt(t)->smlp_lock_arg = (void*)lock_arg; // So we can retrieve it later

		if( sem->fqsize >= num_online_cpus() ) {
			// Go to PQ
			bheap_insert(gsnedf.order, &sem->pq, tsk_rt(t)->smlp_pq_node);
		} else {
			// Go to FQ
			init_waitqueue_entry(&tsk_rt(t)->smlp_fq_node, t);
			sem->fqsize++; // Record the number of FQ'd items, must be less than num cores
			__add_wait_queue_entry_tail_exclusive(&sem->fq, &tsk_rt(t)->smlp_fq_node);
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

		// The lock arg will now keep track of the litmus_lock
		tsk_rt(t)->smlp_lock_arg = (void*)l;
	} else {
		// We can be immediately satisfied, go to SQ
		init_waitqueue_entry(&tsk_rt(t)->smlp_sq_node, t);
		__add_wait_queue_entry_tail_exclusive(&sem->sq, &tsk_rt(t)->smlp_sq_node);

		// Assign the TPCs
		tsk_rt(t)->smlp_assigned_mask = get_smlp_mask(sem->mask, lock_arg->allowed_tpc_bits);
		// Clear out these bits from the available mask
		sem->mask &= ~(tsk_rt(t)->smlp_assigned_mask);
		// Return the assigned mask to the user
		lock_arg->assigned_mask = tsk_rt(t)->smlp_assigned_mask;

		tsk_rt(t)->smlp_lock_arg = (void*)l; // So we can retrieve it later when this task goes to piq

		// unlock and let this task proceed
		spin_unlock_irqrestore(&sem->fq.lock, flags);
	}
    

	// Update the number of locks held, and continue to critical-section
	tsk_rt(t)->num_locks_held++;
	return 0;
}

int gsnedf_smlp_lock_arg(struct litmus_lock* l, void* __user arg) {
	struct smlp_lock_arg lock_arg;
	int rv;

    if( !access_ok(arg, sizeof(lock_arg)) ) {
        return -EPERM;
	}

    if( __copy_from_user(&lock_arg, arg, sizeof(lock_arg)) ) {
        return -EFAULT;
	}

	rv = __gsnedf_smlp_lock(l, &lock_arg);

	if( __copy_to_user(arg, &lock_arg, sizeof(lock_arg)) ) {
		return -EFAULT;
	}

	return rv;
}

// If you call litmus_lock without an arg for the SMLP, it assumes it will
// only lock one TPC at most. Additionally, the assigned mask is filled in
// the control page variable. Note this will cause a race condition if multiple
// lock ops are returned this way, so this is just a fallback not meant to be
// used for tasks that have multiple threads locking SMs.
int gsnedf_smlp_lock(struct litmus_lock* l) {
	struct smlp_lock_arg lock_arg;
	struct task_struct *t = current;
	int rv;
	lock_arg.allowed_tpc_bits = 1; // Only need to lock 1 TPC for default lock op
	rv = __gsnedf_smlp_lock(l, &lock_arg);
	tsk_rt(t)->ctrl_page->smlp_assigned_mask = lock_arg.assigned_mask;
	return rv;
}

int gsnedf_smlp_unlock(struct litmus_lock* l) {
	struct task_struct *t = current, *nextfq, *pqtask, *piqhead;
	struct bheap_node *nextpq;
	struct smlp_semaphore *sem = smlp_from_lock(l);
	struct smlp_lock_arg *lock_arg;
	unsigned long flags;
	struct list_head *pos;
	int bFound;

	// Grab the FQ lock
	spin_lock_irqsave(&sem->fq.lock, flags);

	// Restore the mask of available TPCs
	sem->mask |= tsk_rt(t)->smlp_assigned_mask;
	tsk_rt(t)->smlp_assigned_mask = 0;

	// Are we not in the PIQ?
	bFound = 0;
	list_for_each(pos, &sem->piq.head) {
		if( list_entry( pos, wait_queue_entry_t, entry) == &tsk_rt(t)->smlp_piq_node ) {
			bFound = 1;
			break;
		}
	}

	if( !bFound ) {
		// Two reasons:
		//  person who called unlock doesn't hold the lock
		//  the task never suspended on the CPU
		TRACE_CUR("smlp_unlock called without suspending during critical-section (never entered PIQ)\n");
		
		// Remove ourselves from the SQ
		list_del(&tsk_rt(t)->smlp_sq_node.entry);
	} else {
		// Remove ourselves from the PIQ
		list_del(&tsk_rt(t)->smlp_piq_node.entry);
	}

	tsk_rt(t)->smlp_piq_node.entry.next = NULL;
	tsk_rt(t)->smlp_piq_node.entry.prev = NULL;

	// TODO: move the queues forward
	// Can only do this if there are available TPCs
	while( sem->mask ) {
		// Is anyone in the FQ?
		nextfq = __waitqueue_remove_first(&sem->fq);
		if( !nextfq ) break; // nothing left in FQ, so we are done.

		// Note the lock_arg contains the lock arguments, this needs to be used then set to NULL before
		// the task resumes, otherwise the lock_arg will be assumed to be a litmus_lock pointer.
		lock_arg = (struct smlp_lock_arg*) tsk_rt(nextfq)->smlp_lock_arg;
		tsk_rt(nextfq)->smlp_lock_arg = NULL;

		// Satisfy this request
		init_waitqueue_entry(&tsk_rt(nextfq)->smlp_sq_node, t);
		__add_wait_queue_entry_tail_exclusive(&sem->sq, &tsk_rt(nextfq)->smlp_sq_node);

		// Assign the TPCs
		tsk_rt(nextfq)->smlp_assigned_mask = get_smlp_mask(sem->mask, lock_arg->allowed_tpc_bits);
		// Clear out these bits from the available mask
		sem->mask &= ~(tsk_rt(nextfq)->smlp_assigned_mask);
		// Return the assigned mask to the user
		lock_arg->assigned_mask = tsk_rt(nextfq)->smlp_assigned_mask;

		TRACE_CUR("smlp lock ownership passed to %s/%d\n", nextfq->comm, nextfq->pid);

		// Now add the highest from the PQ to the FQ, if any.
		nextpq = bheap_take(gsnedf.order, &sem->pq);
		if( nextpq ) {
			// Insert this into the FQ tail
			pqtask = bheap2task(nextpq);
			init_waitqueue_entry(&tsk_rt(pqtask)->smlp_fq_node, pqtask );
			__add_wait_queue_entry_tail_exclusive(&sem->fq, &tsk_rt(pqtask)->smlp_fq_node);
		} else {
			sem->fqsize--;
		}

		wake_up_process(nextfq);
	}

	// If we are the hp_waiter, we need to update it
	if( sem->hp_waiter == t ) {
		TRACE_TASK(t, "was highest-prio smlp waiter\n");
		sem->hp_waiter = smlp_find_hp_waiter(sem, t);
		if (sem->hp_waiter) {
			TRACE_TASK(sem->hp_waiter, "is new highest-prio smlp waiter\n");
			// apply priority inheritance to the new hp_waiter if PIQ is active
			if ( waitqueue_active(&sem->piq) ) { // Is anyone in the PIQ?
				piqhead = __waitqueue_peek_first(&sem->piq);
				// If the PIQ head isn't the highest prio, and could use the hp_waiter's prio
				if ( piqhead != sem->hp_waiter // Is the head of the PIQ not the highest priority task found?
					&& edf_higher_prio(sem->hp_waiter, piqhead )
				) {
					gsnedf_set_priority_inheritance( piqhead, sem->hp_waiter );
				}
			} // wq_active
		} // if hp_waiter
	} // if we are the hp_waiter

	// Don't forget to clear out the smlp lock arg.
	tsk_rt(t)->smlp_lock_arg = NULL;

	// We need to remove our own prio inheritance (if any)
	if( tsk_rt(t)->inh_task )
		gsnedf_clear_priority_inheritance(t);
	spin_unlock_irqrestore(&sem->fq.lock, flags);

	return 0;
}


int gsnedf_smlp_close(struct litmus_lock* l) {
	struct task_struct *t = current;
	struct smlp_semaphore *sem = smlp_from_lock(l);
	unsigned long flags;

	// First, check to see if we own the lock, if so, we need to unlock
	int owner;
	spin_lock_irqsave(&sem->fq.lock,flags);
	owner = tsk_rt(t)->smlp_assigned_mask != 0;
	spin_unlock_irqrestore(&sem->fq.lock,flags);

	if( owner )
		gsnedf_smlp_unlock(l);
	return 0;
}

void gsnedf_smlp_free(struct litmus_lock* lock) {
	kfree(smlp_from_lock(lock));
}

static struct litmus_lock_ops gsnedf_smlp_lock_ops = {
	.close  = gsnedf_smlp_close,
	.lock   = gsnedf_smlp_lock,
	.unlock = gsnedf_smlp_unlock,
	.deallocate = gsnedf_smlp_free,
	.lock_arg = gsnedf_smlp_lock_arg,
};

struct litmus_lock* gsnedf_new_smlp(void* __user config) {
	struct smlp_semaphore* sem;
    struct smlp_create_config init_config;

    if( !access_ok(config, sizeof(init_config)) ) {
        return NULL;
	}
    if( __copy_from_user(&init_config, config, sizeof(init_config)) ) {
        return NULL;
	}

	// Allocate the semaphore
	sem = kmalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem) {
		return NULL;
	}

	// initialize lock parameters
	sem->hp_waiter = NULL;
	sem->fqsize = 0;

	bheap_init(&sem->pq);
	init_waitqueue_head(&sem->fq);
    init_waitqueue_head(&sem->sq);
    init_waitqueue_head(&sem->piq);

    sem->mask = init_config.init_mask;
	sem->explicit_gpu_finish = init_config.explicit_gpu_finish;
	sem->litmus_lock.ops = &gsnedf_smlp_lock_ops;
	return &sem->litmus_lock;
}