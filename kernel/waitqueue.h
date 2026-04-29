/*
 * waitqueue.h — Hash-bucketed wait queue for xv6
 *
 * Replaces the O(N) scan in wakeup() with an O(k) hash-table lookup.
 * Inspired by Linux kernel wait_queue_head_t (include/linux/wait.h).
 *
 * Lock ordering (3 tiers — must always be acquired in this order):
 *
 *   caller's lock (lk)  →  bucket lock  →  p->lock
 *           ①                   ②              ③
 *
 * Rules:
 *   - NEVER acquire bucket lock while holding p->lock
 *   - NEVER acquire lk while holding bucket lock
 *   - Only valid order: lk → bucket_lock → p->lock
 */

#ifndef _WAITQUEUE_H_
#define _WAITQUEUE_H_

#include "types.h"
#include "spinlock.h"

// Number of hash buckets — must be power-of-2.
// With NPROC=64, 16 buckets gives ~4 entries/bucket on average.
#define NWQUEUE 16

// Forward declaration
struct proc;

// Wait queue entry — embedded in struct proc (one per process).
// A process can only sleep on one channel at a time, so one entry suffices.
struct wq_entry {
  struct proc    *p;      // owning process
  void           *chan;   // channel this entry is sleeping on
  struct wq_entry *next;  // next entry in the same bucket
};

// One bucket of the hash table — has its own spinlock for fine-grained locking.
struct wq_bucket {
  struct spinlock lock;   // per-bucket lock
  struct wq_entry *head;  // head of singly-linked list of entries
};

// The global wait queue hash table.
struct wq_table {
  struct wq_bucket buckets[NWQUEUE];
};

// Global wait queue table instance (defined in waitqueue.c).
extern struct wq_table wq_table;

// ---------- Hash function ----------

// Hash a channel pointer to a bucket index.
// Shift right 3 bits because kernel pointers are typically 8-byte aligned,
// so the low 3 bits are always 0 and carry no entropy.
// Mask with (NWQUEUE-1) instead of modulo — faster on RISC-V (no division).
static inline int
wq_hash(void *chan)
{
  return (int)(((uint64)chan >> 3) & (NWQUEUE - 1));
}

// ---------- Fast path (lockless check) ----------

// Check if a bucket (for the given channel) has any sleepers.
// Does NOT acquire the bucket lock — used as a fast path to skip
// wakeup() when the bucket is empty.
//
// Memory ordering:
//   - volatile: prevents compiler from caching 'head' in a register
//   - __sync_synchronize(): emits RISC-V "fence rw,rw" to prevent
//     CPU from reordering the read past subsequent instructions
//
// May return false positives (another channel hashes to same bucket),
// but never false negatives when properly ordered. False positives
// only cost one extra lock acquire — correctness is not affected.
static inline int
wq_has_sleeper(void *chan)
{
  int idx = wq_hash(chan);
  struct wq_entry *volatile *hp = &wq_table.buckets[idx].head;
  struct wq_entry *head = *hp;
  __sync_synchronize();
  return head != 0;
}

// ---------- Extern function declarations ----------

// Initialize all bucket locks. Must be called once during boot (from main.c).
void wq_init(void);

// Add entry to the wait queue for the given channel.
// Acquires and releases bucket lock internally.
void wq_enqueue(struct wq_entry *e, void *chan);

// Wake ALL entries sleeping on the given channel.
// Acquires bucket lock, then p->lock for each matching entry.
// Autoremove: entries are dequeued as part of wakeup.
void wq_dequeue(void *chan);

// Wake only the FIRST entry sleeping on the given channel.
// Used to avoid thundering herd (e.g., in pipe read/write).
// Acquires bucket lock, then p->lock for the matching entry.
void wq_dequeue_one(void *chan);

#endif // _WAITQUEUE_H_
