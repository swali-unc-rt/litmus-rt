#ifndef _LITMUS_LOCKING_SMLP_H_
#define _LITMUS_LOCKING_SMLP_H_
#ifdef CONFIG_LITMUS_LOCKING_SMLP

#include <litmus/locking.h>
#include <litmus/rt_param.h>

#include <linux/types.h>

struct smlp_create_config {
    // Initial mask to assign
    // If bit n is set, then TPC n is up for grabs
    uint64_t init_mask;
    // If set, then you don't move to PIQ until explicitly calling LRT_smlp_gpu_done
    int explicit_gpu_finish;
};

struct smlp_lock_arg {
    // Allowed number of TPCs. If bit x is set, then x TPCs are allowed
    uint64_t allowed_tpc_bits;
    uint64_t assigned_mask;
};

struct litmus_lock* gsnedf_new_smlp(void* __user init_config);
long gsnedf_smlp_on_admit_task(struct task_struct * tsk);
void gsnedf_smlp_on_exit_task(struct task_struct * tsk);
void gsnedf_smlp_on_task_arrival(struct task_struct * tsk);
int gsnedf_smlp_on_gpu_done(struct litmus_lock *l);

#endif
#endif