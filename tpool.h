#ifndef _T_POOL_
#define _T_POOL_

int tpool_init(void (*task)(int));
int tpool_add_task(int task_fd);

#endif
