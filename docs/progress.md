# Progress Tracker — Sleep/Wakeup Optimization

> Nhìn vào file này là biết ngay: đã làm gì, đang làm gì, cần làm gì tiếp.  
> Cập nhật file này mỗi khi hoàn thành một task hoặc có quyết định thiết kế mới.

---

## Trạng thái tổng quan

| Sprint | Nội dung | Trạng thái |
|--------|----------|------------|
| Sprint 1 | Setup, đọc codebase, thiết kế struct | ✅ DONE |
| Sprint 2 | Implement wait queue, rewrite sleep/wakeup | ⚠️ DONE (có bug) |
| Sprint 3 | wakeup_one, benchmark, usertests | ✅ DONE |
| Sprint 4 (tuần này) | pipe.c optimization, stress_wakeup | 🔄 IN PROGRESS |
| Sprint 5 | Fix bugs, báo cáo, demo | ⏳ TODO |

---

## ✅ Đã hoàn thành (Sprint 1–4)

### Infrastructure — `kernel/waitqueue.h` + `kernel/waitqueue.c`
- `wq_entry` (proc, chan, next, queued), `wq_bucket` (spinlock, head), `waitqueue_table` (16 buckets)
- `wq_hash(chan)` — `(chan >> 3) & (NWQUEUE-1)`, power-of-2 bitmask
- `wq_init()` — init 16 bucket spinlocks
- `wq_bucket_for(chan)` — trả về bucket tương ứng
- `wq_add_locked(b, chan, p)` — O(1) prepend, set `queued=1`
- `wq_remove_locked(b, entry)` — O(k) remove, clear queued/next/chan
- `wq_has_sleeper(chan)` — fast path check (**xem bug #2 bên dưới**)

### `struct proc` — `kernel/proc.h:108`
- Embedded `struct wq_entry wq_entry` — không cần kalloc, 1 entry/proc

### `wakeup_one()` — `kernel/proc.c:603`
- Wake đúng 1 process trên chan, dừng sau khi tìm được (anti-thundering herd)
- Khai báo trong `defs.h`
- `usertests` pass 100% sau khi thêm

### `user/bench_ipc.c`
- Ping-pong benchmark: 1000 round trips qua pipe, đo ticks + rounds/tick
- Đăng ký trong Makefile, build thành `user/_bench_ipc`
- Chạy trong xv6 shell: `$ bench_ipc`

### `sleep()` rewrite — `kernel/proc.c:552`
- Lock ordering: acquire bucket lock → acquire p->lock → release lk
- Enqueue → set chan + SLEEPING → release bucket lock → sched()
- Cleanup sau sched(): p->chan=0, release p->lock, acquire lk
- **Lost wakeup được prevent**: lk chỉ release sau khi đã giữ bucket lock

### `wakeup()` rewrite — `kernel/proc.c:579`
- O(k) scan — chỉ duyệt bucket tương ứng với hash(chan)
- Acquire p->lock per entry, check SLEEPING && chan match
- Set RUNNABLE + `wq_remove_locked()` (autoremove pattern)

### Build system
- `Makefile` — `kernel/waitqueue.o` đã được add vào OBJS (line 15 và 28)

---

## ⏳ TODO

### P1 — Core features còn thiếu

- [ ] **`pipe.c` optimization** — hiện vẫn là xv6 gốc, chưa tối ưu gì:
  - Thay `wakeup()` → `wakeup_one()` (3 chỗ: pipewrite full, pipewrite done, piperead done)
  - Thêm `wq_has_sleeper()` fast path trước mỗi `wakeup_one()`
  - Batch `copyin()` ra ngoài lock (từng byte → cả chunk)

### P2 — Benchmark & stress test

- [ ] **`user/stress_wakeup.c`** — stress test 20+ process cùng sleep/wakeup
  - Verify không deadlock, không lost wakeup

### P3 — Verification & report

- [ ] **Chạy `grind`** — multi-process stress
- [ ] **Benchmark so sánh** — ghi lại số liệu baseline vs optimized (chạy bench_ipc trước/sau pipe.c opt)
- [ ] Báo cáo kỹ thuật, demo script

---

## 🐛 Open Bugs / Issues

### ~~Bug #1 — `defs.h`: `wq_init` khai báo trùng~~ ✅ Fixed
- **File:** `kernel/defs.h` line 106 và 188
- **Fix:** Xóa khai báo thứ 2 ở cuối file

### ~~Bug #2 — `wq_has_sleeper()` vẫn acquire lock~~ ✅ Fixed
- **File:** `kernel/waitqueue.c:62`
- **Fix:** Thay acquire/release bằng `__atomic_load_n(&b->head, __ATOMIC_ACQUIRE) != 0`
- **Lưu ý:** False positive (bucket có entry nhưng không phải chan này) là acceptable — correctness vẫn đúng

### ~~Bug #3 — `wq_init()` gọi 2 lần khi boot~~ ✅ Fixed
- **File:** `kernel/main.c:23` và `kernel/proc.c:55`
- **Fix:** Xóa `wq_init()` khỏi `main.c`, giữ trong `procinit()`

### Bug #4 — `kkill()` potential stale-chan race
- **File:** `kernel/proc.c:614-619`
- **Vấn đề:** Đọc `p->chan` sau khi release `p->lock`, trước khi call `wakeup(chan)` — giữa 2 bước đó chan có thể đã stale
- **Tác hại:** Low severity — spurious wakeup trên wrong channel, nhưng process đã bị set `killed=1` nên sẽ tự exit
- **Fix:** Không urgent, nhưng có thể pass chan trực tiếp vào wakeup trong khi vẫn giữ p->lock bằng cách gọi `wq_bucket_for` + manual wake

---

## 📋 Quyết định thiết kế đã chốt

| Quyết định | Lý do |
|-----------|-------|
| 16 buckets (NWQUEUE=16) | NPROC=64 → ~4 entries/bucket, đủ O(k) |
| Singly-linked list (không doubly-linked) | Max 64 procs, singly đủ nhanh, đơn giản hơn |
| Embedded `wq_entry` trong `struct proc` | Mỗi proc chỉ sleep 1 channel tại 1 thời điểm, tránh kalloc (4KB/page quá lớn cho 24 bytes) |
| Autoremove pattern (dequeue trong wakeup) | Self-remove trong sleep gây false positive cho wq_has_sleeper |
| Release `lk` INSIDE sleep sau khi giữ bucket lock | Prevent lost wakeup — wakeup phải chờ bucket lock |
| Hash: `chan >> 3` bỏ 3 bit alignment | Aligned pointers có 3 bit cuối = 0, hash sẽ cluster vào bucket 0 nếu không shift |

---

### ~~Bug #5 — `Makefile`: `waitqueue.o` linked 2 lần~~ ✅ Fixed
- **File:** `Makefile` line 15 và 28
- **Tác hại:** Linker error — multiple definition của tất cả symbols trong waitqueue.c
- **Fix:** Xóa dòng 28

---

## 📝 Context cho lần làm tiếp

**Next action:** Sửa bug #1 và #2 trước, rồi implement `wakeup_one()`, rồi optimize `pipe.c`.

**Lock ordering tuyệt đối không được vi phạm:**
```
caller's lock (lk)  →  bucket lock  →  p->lock
```
`sched()` yêu cầu `noff == 1` → phải release bucket lock trước khi gọi sched().

**`pipe.c` cần wakeup_one, không phải wakeup** — pipe writer/reader là 1-to-1, wake all gây thundering herd không cần thiết.

**Chưa test gì cả** — build có thể pass nhưng chưa chạy `usertests` lần nào trên codebase hiện tại.
