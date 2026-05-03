#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "waitqueue.h"

struct waitqueue_table wt;

int
wq_hash(void *chan)
{
  return ((uint64)chan >> 3) & (NWQUEUE - 1);
}

struct wq_bucket*
wq_bucket_for(void *chan)
{
  return &wt.buckets[wq_hash(chan)];
}

void
wq_init(void)
{
  for(int i = 0; i < NWQUEUE; i++){
    initlock(&wt.buckets[i].lock, "wq_bucket");
    wt.buckets[i].head = 0;
  }
}

void
wq_add_locked(struct wq_bucket *b, void *chan, struct proc *p)
{
  p->wq_entry.proc = p;
  p->wq_entry.chan = chan;
  p->wq_entry.next = b->head;
  p->wq_entry.queued = 1;
  b->head = &p->wq_entry;
}

void
wq_remove_locked(struct wq_bucket *b, struct wq_entry *entry)
{
  struct wq_entry **pp = &b->head;

  while(*pp){
    if(*pp == entry){
      *pp = entry->next;
      entry->next = 0;
      entry->chan = 0;
      entry->queued = 0;
      return;
    }
    pp = &(*pp)->next;
  }
}

int
wq_has_sleeper(void *chan)
{
  struct wq_bucket *b = wq_bucket_for(chan);
  return __atomic_load_n(&b->head, __ATOMIC_ACQUIRE) != 0;
}
