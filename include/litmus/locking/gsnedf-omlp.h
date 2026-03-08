#ifndef _LITMUS_LOCKING_OMLP_H_
#define _LITMUS_LOCKING_OMLP_H_
#ifdef CONFIG_LITMUS_LOCKING_OMLP

#include <litmus/locking/locking.h>
#include <litmus/rt_param.h>

struct litmus_lock* gsnedf_new_omlp(void);
long gsnedf_omlp_on_admit_task(struct task_struct * tsk);
void gsnedf_omlp_on_exit_task(struct task_struct * tsk);

#endif
#endif