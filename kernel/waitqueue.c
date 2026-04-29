/*
 * waitqueue.c — Hash-bucketed wait queue implementation for xv6
 *
 * Provides O(k) wakeup instead of O(N) scan of the entire process table.
 * See waitqueue.h for struct definitions and lock ordering rules.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "waitqueue.h"

// The single global wait queue hash table.
struct wq_table wq_table;

// ---------- Initialization ----------

// Initialize all 16 bucket locks.
// Called once from main() during boot, before scheduling begins.
void
wq_init(void)
{
  for (int i = 0; i < NWQUEUE; i++) {
    initlock(&wq_table.buckets[i].lock, "wq_bucket");
    wq_table.buckets[i].head = 0;
  }
}

// ---------- Enqueue ----------

// Add a wait queue entry for the given channel.
// Prepends to the head of the bucket's singly-linked list (O(1)).
// Acquires and releases the bucket lock internally.
//
// Caller must initialize e->p before calling (typically e->p = myproc()).
// This function sets e->chan and e->next.
void
wq_enqueue(struct wq_entry *e, void *chan)
{
  int idx = wq_hash(chan);
  struct wq_bucket *b = &wq_table.buckets[idx];

  acquire(&b->lock);
  e->chan = chan;
  e->next = b->head;
  b->head = e;
  release(&b->lock);
}

// ---------- Dequeue all (wakeup) ----------

// Wake ALL entries in the bucket that match the given channel.
// For each matching entry:
//   1. Remove from the linked list (autoremove)
//   2. Acquire p->lock (lock ordering: bucket_lock → p->lock)
//   3. If p->state == SLEEPING, set p->state = RUNNABLE
//   4. Release p->lock
//
// This is the "wake all" variant — used by wakeup() in proc.c.
void
wq_dequeue(void *chan)
{
  int idx = wq_hash(chan);
  struct wq_bucket *b = &wq_table.buckets[idx];

  acquire(&b->lock);

  struct wq_entry **pp = &b->head;  // pointer to pointer for easy removal
  while (*pp) {
    struct wq_entry *e = *pp;
    if (e->chan == chan) {
      // Remove from list
      *pp = e->next;
      e->next = 0;
      e->chan = 0;

      // Wake the process (bucket_lock → p->lock ordering)
      acquire(&e->p->lock);
      if (e->p->state == SLEEPING) {
        e->p->state = RUNNABLE;
      }
      release(&e->p->lock);
    } else {
      pp = &e->next;
    }
  }

  release(&b->lock);
}

// ---------- Dequeue one (wakeup_one) ----------

// Wake only the FIRST entry in the bucket that matches the given channel.
// Used to avoid thundering herd — e.g., pipe only needs to wake one reader/writer.
//
// Same lock ordering as wq_dequeue: bucket_lock → p->lock.
void
wq_dequeue_one(void *chan)
{
  int idx = wq_hash(chan);
  struct wq_bucket *b = &wq_table.buckets[idx];

  acquire(&b->lock);

  struct wq_entry **pp = &b->head;
  while (*pp) {
    struct wq_entry *e = *pp;
    if (e->chan == chan) {
      // Remove from list
      *pp = e->next;
      e->next = 0;
      e->chan = 0;

      // Wake the process
      acquire(&e->p->lock);
      if (e->p->state == SLEEPING) {
        e->p->state = RUNNABLE;
      }
      release(&e->p->lock);

      // Only wake one — done
      release(&b->lock);
      return;
    }
    pp = &e->next;
  }

  release(&b->lock);
}
