#include <litmus/fdso.h>
#include <litmus/litmus.h>
#include <litmus/rt_domain.h>

#include <linux/list.h>

#include <litmus/releasegroups/releasegroups.h>
#include <litmus/sched/sched_plugin.h>
#include <litmus/tracing/debug_trace.h>
#include <litmus/tracing/sched_trace.h>

#include <litmus/jobs.h>

struct releasegroup_environment rgenv;
raw_spinlock_t rgenv_lock;

void releasegroup_init(struct releasegroup* rgroup) {
    memset(rgroup, 0, sizeof(*rgroup));
    INIT_LIST_HEAD(&rgroup->tasks);
}

void releasegroup_environment_init() {
    memset(&rgenv, 0, sizeof(rgenv));
    INIT_LIST_HEAD(&rgenv.groups);
    raw_spin_lock_init(&rgenv_lock);
}

void releasegroup_environment_destroy() {
    struct releasegroup *rg, *next;
    struct rt_param *rt, *rtnext;
    struct task_struct *ts;
    lt_t now;
    unsigned long flags;

    raw_spin_lock_irqsave(&rgenv_lock, flags);

    now = litmus_clock();

    list_for_each_entry_safe(rg, next, &rgenv.groups, list) {
        list_for_each_entry_safe(rt, rtnext, &rg->tasks, releasegroup_entry ) {
            list_del(&rt->releasegroup_entry);
            rt->releasegroup_id = 0;
            rt->cached_releasegroup = 0;
            // re-enter them into the scheduler so this process can close
            ts = container_of(rt, struct task_struct, rt_param);
            release_at(ts,now);
            litmus->task_wake_up( ts );
        }
        list_del(&rg->list);
        kfree(rg);
    }

    memset(&rgenv, 0, sizeof(rgenv));
    INIT_LIST_HEAD(&rgenv.groups);
    raw_spin_unlock_irqrestore(&rgenv_lock, flags);
}

long create_releasegroup(unsigned int id) {
    struct releasegroup *rg;
    unsigned long flags;

    if( 0 == id ) {
        // We do not allow group IDs of zero, cause that means
        // not in a group
        TRACE_TASK(current, "cannot create release group id %u\n", id);
        return -EINVAL;
    }

    raw_spin_lock_irqsave(&rgenv_lock, flags);

    if( NULL != find_releasegroup(id) ) {
        // error, cannot recreate this release group
        TRACE_TASK(current, "release group id %u already exists!\n", id);
        raw_spin_unlock_irqrestore(&rgenv_lock, flags);
        return -EINVAL;
    }

    rg = kzalloc(sizeof(*rg), GFP_KERNEL);
    if( !rg ) {
        raw_spin_unlock_irqrestore(&rgenv_lock, flags);
        return -ENOMEM;
    }
    releasegroup_init(rg);
    rg->id = id;
    
    // add to the list
    list_add_tail(&rg->list, &rgenv.groups);

    raw_spin_unlock_irqrestore(&rgenv_lock, flags);

    TRACE_TASK(current, "created release group id %u\n", id);
    return 0;
}

long add_task_to_releasegroup(unsigned int rgroup_id, struct task_struct* ts) {
    unsigned long flags;
    struct releasegroup *rg;

    if( 0 == rgroup_id ) {
        // We do not allow group IDs of zero, cause that means
        // not in a group
        TRACE_TASK(ts, "cannot be added to release group id %u\n", rgroup_id);
        return -EINVAL;
    }

    raw_spin_lock_irqsave(&rgenv_lock, flags);

    rg = find_releasegroup(rgroup_id);
    if( NULL == rg ) {
        // Could not find release group
        TRACE_TASK(ts, "could not be added to unknown release group %u\n", rgroup_id);
        raw_spin_unlock_irqrestore(&rgenv_lock, flags);
        return -EINVAL;
    }

    list_add_tail(&(tsk_rt(ts)->releasegroup_entry), &rg->tasks);
    tsk_rt(ts)->releasegroup_id = rgroup_id;

    raw_spin_unlock_irqrestore(&rgenv_lock, flags);

    return 0;
}

long remove_task_from_releasegroup(struct task_struct* ts) {
    struct releasegroup *rg;
    struct rt_param *rt, *next;
    unsigned long flags;

    if( 0 == tsk_rt(ts)->releasegroup_id ) {
        // Not in a release group
        TRACE_TASK(ts,"is not in a release group\n");
        return 0;
    }

    raw_spin_lock_irqsave(&rgenv_lock, flags);

    rg = find_releasegroup(tsk_rt(ts)->releasegroup_id);
    if( NULL == rg ) {
        // Could not find release group
        raw_spin_unlock_irqrestore(&rgenv_lock, flags);
        TRACE_TASK(ts,"could not find release group %u when removing self from group", tsk_rt(ts)->releasegroup_id );
        return -EINVAL;
    }

    list_for_each_entry_safe(rt, next, &rg->tasks, releasegroup_entry) {
        if( rt == tsk_rt(ts) ) {
            list_del(&rt->releasegroup_entry);
            tsk_rt(ts)->releasegroup_id = 0;
            raw_spin_unlock_irqrestore(&rgenv_lock, flags);
            return 0;
        }
    }

    raw_spin_unlock_irqrestore(&rgenv_lock, flags);

    TRACE_TASK(ts,"could not find task in release group %u for deletion\n", tsk_rt(ts)->releasegroup_id );
    return -EINVAL;
}

struct releasegroup* find_releasegroup(unsigned int id) {
    struct releasegroup *rg, *next;

    list_for_each_entry_safe(rg, next, &rgenv.groups, list) {
        if( rg->id == id )
            return rg;
    }

    return 0;
}

asmlinkage long sys_releasegroup_release(unsigned int releasegroup_id) {
    struct releasegroup *rg;
    unsigned long flags;

    raw_spin_lock_irqsave(&rgenv_lock, flags);
    rg = find_releasegroup(releasegroup_id);
    raw_spin_unlock_irqrestore(&rgenv_lock, flags);

    if( NULL == rg ) {
        // Could not find release group
        TRACE_TASK(current, "could not find release group %u for release\n", releasegroup_id);
        return -EINVAL;
    }

    return litmus->releasegroup_release(rg);
}

asmlinkage long sys_releasegroup_create(unsigned int releasegroup_id) {
    return create_releasegroup(releasegroup_id);
}

asmlinkage long sys_releasegroup_addtask(unsigned int releasegroup_id) {
    return add_task_to_releasegroup(releasegroup_id, current);
}

asmlinkage long sys_releasegroup_remove(void) {
    return litmus->releasegroup_remove ? litmus->releasegroup_remove() : -ENOSYS;
}

asmlinkage long sys_releasegroup_envinit(void) {
    releasegroup_environment_init();
    return 0;
}

asmlinkage long sys_releasegroup_envdestroy(void) {
    releasegroup_environment_destroy();
    return 0;
}

asmlinkage long sys_releasegroup_cache(unsigned int releasegroup_id) {
    struct releasegroup *rg;
    unsigned long flags;

    raw_spin_lock_irqsave(&rgenv_lock, flags);

    rg = find_releasegroup(releasegroup_id);
    if( NULL == rg ) {
        // Could not find release group
        TRACE_TASK(current, "could not find release group %u for caching\n", releasegroup_id);
        raw_spin_unlock_irqrestore(&rgenv_lock, flags);
        return -EINVAL;
    }

    tsk_rt(current)->cached_releasegroup = rg;
    raw_spin_unlock_irqrestore(&rgenv_lock, flags);
    return 0;
}

asmlinkage long sys_releasegroup_release_cached(void) {
    return litmus->releasegroup_release ? litmus->releasegroup_release(tsk_rt(current)->cached_releasegroup) : -ENOSYS;
}