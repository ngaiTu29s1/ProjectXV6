#ifndef _WAITQUEUE_H_
#define _WAITQUEUE_H_

#define NWQUEUE 16

struct proc;

struct wq_entry {
  struct proc *proc;
  void *chan;
  struct wq_entry *next;
  int queued;
};

struct wq_bucket {
  struct spinlock lock;
  struct wq_entry *head;
};

struct waitqueue_table {
  struct wq_bucket buckets[NWQUEUE];
};

extern struct waitqueue_table wt;

int             wq_hash(void*);
void            wq_init(void);
struct wq_bucket* wq_bucket_for(void*);
void            wq_add_locked(struct wq_bucket*, void*, struct proc*);
void            wq_remove_locked(struct wq_bucket*, struct wq_entry*);
int             wq_has_sleeper(void*);

#endif
