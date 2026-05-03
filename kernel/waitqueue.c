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
    // Release store so wq_has_sleeper()'s acquire load sees a consistent zero.
    __atomic_store_n(&wt.buckets[i].head, (struct wq_entry*)0, __ATOMIC_RELEASE);
  }
}

void
wq_add_locked(struct wq_bucket *b, void *chan, struct proc *p)
{
  p->wq_entry.proc = p;
  p->wq_entry.chan = chan;
  // Acquire the current head so the new entry's next pointer is consistent,
  // then publish the new head with a release store for wq_has_sleeper().
  p->wq_entry.next = __atomic_load_n(&b->head, __ATOMIC_ACQUIRE);
  p->wq_entry.queued = 1;
  __atomic_store_n(&b->head, &p->wq_entry, __ATOMIC_RELEASE);
}

void
wq_remove_locked(struct wq_bucket *b, struct wq_entry *entry)
{
  struct wq_entry *head = __atomic_load_n(&b->head, __ATOMIC_ACQUIRE);

  if(head == entry){
    // Head removal: must use atomic store so wq_has_sleeper() sees it.
    __atomic_store_n(&b->head, entry->next, __ATOMIC_RELEASE);
    entry->next = 0;
    entry->chan = 0;
    entry->queued = 0;
    return;
  }
  // Non-head removal: only entry->next pointers change, not b->head.
  struct wq_entry *e = head;
  while(e && e->next){
    if(e->next == entry){
      e->next = entry->next;
      entry->next = 0;
      entry->chan = 0;
      entry->queued = 0;
      return;
    }
    e = e->next;
  }
}

int
wq_has_sleeper(void *chan)
{
  struct wq_bucket *b = wq_bucket_for(chan);
  // Lockless fast path: acquire load pairs with release stores in
  // wq_add_locked / wq_remove_locked.  False positives are acceptable
  // (bucket non-empty but no entry matches chan) — correctness is upheld
  // by wakeup_one()'s locked chan check.
  return __atomic_load_n(&b->head, __ATOMIC_ACQUIRE) != 0;
}
