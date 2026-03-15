#ifndef RELEASEGROUPS_H
#define RELEASEGROUPS_H

#ifdef CONFIG_LITMUS_ENABLE_RELEASEGROUPS

#include <linux/list.h>
#include <litmus/rt_param.h>

struct task_struct;

struct releasegroup {
    struct list_head list;
    unsigned int id;

    struct list_head tasks;
};

void releasegroup_init(struct releasegroup* rgroup);

struct releasegroup_environment {
    struct list_head groups;
};

void releasegroup_environment_init(void);
void releasegroup_environment_destroy(void);

long create_releasegroup(unsigned int id);
long add_task_to_releasegroup(unsigned int rgroup_id, struct task_struct* ts);
long remove_task_from_releasegroup(struct task_struct* ts);
struct releasegroup* find_releasegroup(unsigned int id);

#endif /* CONFIG_LITMUS_ENABLE_RELEASEGROUPS */

#endif /* RELEASEGROUPS_H */