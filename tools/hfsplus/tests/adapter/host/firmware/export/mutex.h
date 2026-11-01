#ifndef MUTEX_H
#define MUTEX_H
struct mutex { int dummy; };
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
#endif
