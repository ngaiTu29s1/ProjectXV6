# Thiết kế — Sleep & Wakeup Optimization

> Tài liệu thiết kế Sprint 1. Mô tả ý tưởng, kiến trúc, và quyết định kỹ thuật.

---

## 1. Phân tích vấn đề hiện tại

### Cơ chế sleep/wakeup của xv6 gốc

Xv6 hiện tại sử dụng cơ chế **channel-based sleep/wakeup**:
- Mỗi process có trường `void *chan` — đại diện cho "kênh" đang chờ
- `sleep(chan, lock)` — đặt process vào trạng thái SLEEPING với channel `chan`
- `wakeup(chan)` — duyệt **toàn bộ** process table, đánh thức tất cả process có `chan` trùng

### Các vấn đề cốt lõi

| # | Vấn đề | Nguyên nhân | Hậu quả |
|---|--------|-------------|---------|
| 1 | **O(N) scan** | `wakeup()` duyệt mảng `proc[NPROC]` (64 phần tử) mỗi lần gọi | Lãng phí CPU, tăng latency |
| 2 | **Thundering herd** | Wake tất cả process trên cùng channel, dù chỉ cần 1 | Spurious wakeup, lock contention |
| 3 | **Lock hold time lớn** | `pipewrite()` giữ pipe lock suốt vòng lặp, gọi `copyin()` bên trong | Block reader, tăng IPC latency |
| 4 | **Không có fast path** | Luôn acquire N lock dù không ai đang sleeping | Lãng phí CPU cycles khi không cần thiết |

### Minh họa vấn đề O(N)

```
wakeup(chan_A) phải duyệt:

proc[0]  proc[1]  proc[2]  ...  proc[63]
  ↓        ↓        ↓              ↓
check    check    check    ...   check     ← acquire/release lock 64 lần
  ✗        ✓        ✗              ✗       ← chỉ 1 process cần wake
```

Dù chỉ có **1 process** đang sleep trên `chan_A`, `wakeup()` vẫn phải acquire lock và kiểm tra **tất cả 64 process**.

---

## 2. Ý tưởng giải pháp: Hash-bucketed Wait Queue

### Nguồn cảm hứng — Linux kernel

Linux kernel sử dụng `wait_queue_head_t` (định nghĩa trong `include/linux/wait.h`) — mỗi object (pipe, inode, ...) có riêng một wait queue. Các kỹ thuật chính:

| Kỹ thuật Linux | Ý nghĩa | Cách adapt cho xv6 |
|----------------|---------|---------------------|
| Per-object `wait_queue_head_t` | Mỗi resource có queue riêng | Dùng global hash table thay vì per-object (đơn giản hơn, không cần sửa mọi struct) |
| `prepare_to_wait` → `schedule` → `finish_wait` | Pattern 4 bước tránh lost wakeup | Áp dụng trong flow của `sleep()` |
| `WQ_FLAG_EXCLUSIVE` | Chỉ wake 1 waiter, tránh thundering herd | Implement `wakeup_one()` |
| `wq_has_sleeper()` | Fast path: skip wakeup nếu queue rỗng | Inline check trước khi gọi wakeup |
| Giảm lock hold time trong pipe | Copy data ngoài lock | Batch `copyin()` trước khi acquire pipe lock |

> **Bỏ qua:** Doubly-linked list (`list_head`) — xv6 chỉ có tối đa 64 process, singly-linked đủ nhanh, thêm `prev` pointer làm tăng complexity mà cải thiện hiệu năng không đáng kể.

### Kiến trúc tổng quan

```
                    ┌──────────────────────────────────┐
  sleep(chan, lk)    │   Wait Queue Hash Table          │    wakeup(chan)
        │           │   (16 buckets, per-bucket lock)  │         │
        │           │                                   │         │
        ▼           │  [0]: lock → entry → entry → NULL │         ▼
   hash(chan)──────▶│  [1]: lock → NULL                  │◀────hash(chan)
   tìm bucket       │  [2]: lock → entry → NULL         │   tìm bucket
   enqueue O(1)     │  ...                              │   scan O(k)
                    │  [15]: lock → entry → NULL         │   wake + dequeue
                    └──────────────────────────────────┘
```

**So sánh:**

```
Trước:  wakeup(chan) → scan 64 process → acquire 64 locks       → O(N)
Sau:    wakeup(chan) → hash(chan) → scan ~4 entries → 1 lock     → O(k)
```

---

## 3. Quyết định thiết kế

### 3.1. Tại sao hash table thay vì per-object queue?

Linux nhúng `wait_queue_head_t` vào từng object (pipe, inode, ...). Nếu làm vậy trong xv6, phải sửa `struct pipe`, `struct log`, ... — quá nhiều thay đổi. **Global hash table** là giải pháp "drop-in" thay thế:
- Chỉ cần sửa `sleep()` và `wakeup()`
- Các caller (pipe, console, log, ...) không cần thay đổi
- Trade-off: hash collision → nhiều channel cùng bucket → k lớn hơn lý tưởng

### 3.2. Tại sao NWQUEUE = 16 (power-of-2)?

- **Power-of-2** cho phép dùng **bitmask** (`& (NWQUEUE-1)`) thay vì modulo — nhanh hơn trên RISC-V, tránh phép chia
- **Tại sao 16 thay vì 8 hay 32?** Với `NPROC = 64`, 16 bucket cho trung bình **4 entry/bucket** — đủ nhỏ để scan nhanh (O(4)), nhưng không quá nhiều bucket gây lãng phí bộ nhớ (mỗi bucket có 1 spinlock + 1 pointer). 8 bucket → 8 entry/bucket (scan chậm hơn 2x). 32 bucket → 2 entry/bucket nhưng tốn gấp đôi bộ nhớ metadata với cải thiện không đáng kể
- Hash function: `((uint64)chan >> 3) & (NWQUEUE-1)` — shift phải 3 bit vì pointer kernel thường align 8 bytes (`2^3`), bỏ 3 bit thấp luôn bằng 0 để tăng entropy cho hash

### 3.3. Tại sao embedded entry thay vì dynamic allocation?

- Mỗi process chỉ sleep trên **1 channel** tại một thời điểm → 1 entry/proc là đủ
- `kalloc()` cấp 4KB/page — quá lớn cho 1 entry ~24 bytes
- Embedded entry: không cần memory management, không memory leak, O(1) allocation

### 3.4. Autoremove vs Self-remove

Hai cách xóa entry khỏi queue khi process được đánh thức:

| | Autoremove (chọn) | Self-remove |
|---|---|---|
| **Ai xóa** | `wakeup()` xóa ngay khi set RUNNABLE | `sleep()` tự xóa sau khi được đánh thức |
| **Queue state** | Nhất quán — chỉ chứa process đang SLEEPING | Không nhất quán — chứa cả process đã RUNNABLE |
| **Fast path** | `wq_has_sleeper()` chính xác hơn | False positive → gọi wakeup thừa |
| **Complexity** | Dequeue trong wakeup (đã giữ bucket lock) | Cần re-acquire bucket lock trong sleep |

**Chọn autoremove** vì queue luôn phản ánh đúng trạng thái, fast path chính xác hơn, và `wakeup()` đã giữ bucket lock sẵn.

---

## 4. Lock Ordering

### Thứ tự 3 tầng lock

```
caller's lock (lk)  →  bucket lock  →  p->lock
        ①                   ②              ③
```

Caller (pipe, log, console) giữ `lk` khi gọi `sleep(chan, lk)`. Vì vậy thực tế có **3 tầng lock**, không phải 2.

### Luồng lock trong sleep()

```
sleep(chan, lk):
  [đang giữ lk]
  ① acquire(bucket_lock)     ← bucket lock second
  ② wq_add_locked(entry)     ← enqueue vào queue (vẫn giữ bucket_lock)
  ③ release(lk)              ← safe: wakeup cần bucket_lock trước
  ④ acquire(p->lock)         ← p->lock third
  ⑤ p->state = SLEEPING
  ⑥ release(bucket_lock)     ← sched() chỉ cho giữ p->lock
  ⑦ sched()                  ← yield CPU, scheduler release p->lock
  ⑧ [thức dậy]
  ⑨ release(p->lock)
  ⑩ acquire(lk)              ← re-acquire caller's lock
```

> **Chú ý race window giữa bước ② và ⑤:** Sau `wq_add_locked()`, entry đã nằm trong queue nhưng `p->state` chưa là `SLEEPING`. Nếu `wakeup()` chạy ngay lúc này, liệu có mất wakeup?
>
> **Không**, vì `bucket_lock` bảo vệ: `wakeup()` phải `acquire(bucket_lock)` trước khi duyệt queue — nhưng `sleep()` vẫn đang giữ `bucket_lock` cho đến bước ⑥. Vì vậy `wakeup()` sẽ bị block cho đến khi `sleep()` đã set `SLEEPING` và release bucket lock. Thứ tự xảy ra luôn là: enqueue → set SLEEPING → release bucket_lock → wakeup() thấy SLEEPING → set RUNNABLE.

### Luồng lock trong wakeup()

```
wakeup(chan):
  [đang giữ lk — caller's lock]
  ① acquire(bucket_lock)
  ② duyệt entries trong bucket
  ③   acquire(p->lock)       ← lock ordering ✓
  ④   if SLEEPING → set RUNNABLE + dequeue
  ⑤   release(p->lock)
  ⑥ release(bucket_lock)
```

### Deadlock rules

1. **KHÔNG BAO GIỜ** acquire bucket lock khi đang giữ `p->lock`
2. **KHÔNG BAO GIỜ** acquire `lk` (caller's lock) khi đang giữ bucket lock (trừ khi đã release bucket lock trước)
3. Thứ tự duy nhất hợp lệ: `lk → bucket_lock → p->lock`

---

## 5. Các điểm cần chú ý khi implement

### 5.1. Fast path — `wq_has_sleeper()`

- Kiểm tra bucket `head != NULL` mà **không acquire lock**
- **Memory ordering trên RISC-V:** xv6 không dùng C11 atomics (`<stdatomic.h>`). Built-in `__atomic_load_n(..., __ATOMIC_ACQUIRE)` có thể không portable trên `riscv64-linux-gnu-gcc` target. Hai lựa chọn thay thế:
  - Dùng `volatile` read kết hợp `__sync_synchronize()` (compiler + hardware fence) — xem implementation bên dưới
  - **Bỏ fast path hoàn toàn** và gọi `wakeup_one()` trực tiếp — correctness không bị ảnh hưởng, chỉ thêm chi phí 1 lần acquire bucket lock mỗi lần gọi. Với `NPROC = 64` trên QEMU, chi phí này không đáng kể
- Chỉ kiểm tra **bucket**, không lọc theo `chan` → có thể xảy ra false positive (bucket có entry nhưng không thuộc channel đang tìm)
- False positive không ảnh hưởng correctness — chỉ tốn thêm 1 lần acquire lock
- **Khuyến nghị:** nếu không chắc chắn về memory ordering, bỏ fast path là lựa chọn an toàn nhất
- Nếu chọn giữ fast path, implementation đề xuất:
  ```c
  static inline int
  wq_has_sleeper(void *chan)
  {
    int idx = wq_hash(chan);
    // volatile: ngăn compiler cache giá trị head vào register (buộc đọc từ memory mỗi lần)
    // Lưu ý: volatile KHÔNG đảm bảo memory ordering trên multicore — chỉ là compiler hint
    struct wq_entry *head = *(volatile struct wq_entry **)&wt.buckets[idx].head;
    // __sync_synchronize(): phát ra RISC-V "fence rw,rw" instruction
    // Đảm bảo CPU không reorder read ở trên xuống sau các instruction tiếp theo
    __sync_synchronize();
    return head != 0;
  }
  ```

### 5.2. Pipe optimization — giảm lock hold time

Di chuyển `copyin()` (copy data từ user space) ra **ngoài** pipe lock nhằm giảm thời gian giữ lock:
- `copyin()` có thể chậm do page table walk và page fault
- Hiện tại xv6 gọi `copyin()` từng byte bên trong lock → reader bị block toàn bộ thời gian copy
- Giải pháp: batch copy vào buffer tạm trên kernel stack, sau đó mới acquire lock và ghi vào pipe

**Lưu ý quan trọng:**

- **Stack safety (risk đã được đánh giá và chấp nhận):** Buffer tạm `char buf[PIPESIZE]` chiếm 512 bytes trên kernel stack 4KB (`KSTACKSIZE`), tương đương 12.5%. Chấp nhận được vì:
  1. `pipewrite()` chỉ được gọi từ syscall `sys_write` — không gọi từ interrupt context
  2. Call depth tối đa khoảng 5 frames ≈ 200–400 bytes
  3. Tổng stack usage ước tính: ~400 bytes (call frames) + 512 bytes (buffer) ≈ 912 bytes, còn dư ~3KB
  4. Không tồn tại recursive call path nào dẫn đến `pipewrite()`
- **Xử lý `n > PIPESIZE`:** `pipewrite()` cần một **outer loop** chia `n` thành nhiều chunk, mỗi chunk tối đa `PIPESIZE` bytes. Nếu thiếu outer loop, write lớn hơn 512 bytes sẽ bị cắt ngầm dẫn đến mất dữ liệu. Xv6 gốc xử lý bằng cách copy từng byte nên không gặp vấn đề này, nhưng batch copy bắt buộc phải có loop

### 5.3. `wakeup_one()` — chỉ wake 1 process

- Dùng ở pipe read/write — chỗ chỉ cần wake 1 reader/writer
- **Giữ nguyên** `wakeup()` (wake all) ở `exit()`, `reparent()` — những chỗ cần wake tất cả
- Caller **bắt buộc** phải có retry loop `while(condition) { sleep(...); }` — xv6 pipe đã có sẵn

**Tại sao `wakeup_one()` không gây lost wakeup?**

`wakeup_one()` chỉ wake **1 process đầu tiên** trong bucket có `chan` trùng. Các process còn lại vẫn SLEEPING trong queue. Điều này an toàn vì:
1. Process được wake sẽ chạy, consume resource (đọc/ghi pipe), rồi gọi `wakeup_one()` cho phía đối diện → **chain wakeup**: mỗi consumer wake producer tiếp theo và ngược lại
2. Caller luôn có retry loop `while(!condition) sleep(chan, lk)` — nếu resource chưa sẵn sàng, process quay lại sleep và sẽ được wake khi resource available
3. Nếu không có ai gọi `wakeup_one()` cho các process còn lại, nghĩa là resource chưa available → chúng **nên** tiếp tục sleep, không phải lost wakeup

### 5.4. `piperead()` không dùng fast path — intentional

`pipewrite()` gọi `wq_has_sleeper()` trước `wakeup_one()` để skip wakeup khi bucket rỗng. Nhưng `piperead()` gọi `wakeup_one()` trực tiếp mà không qua fast path. Đây là **intentional** vì:
- `piperead()` luôn gọi `wakeup_one(&pi->nwrite)` **sau khi đã đọc data** — tức là pipe buffer vừa giải phóng space, writer rất có thể đang chờ → xác suất bucket rỗng thấp, fast path hiếm khi skip được
- `pipewrite()` gọi `wakeup_one(&pi->nread)` trong vòng lặp ghi từng chunk — có thể gọi nhiều lần liên tiếp mà reader chưa kịp sleep lại → fast path giúp skip các lần gọi thừa
- Nếu muốn thêm fast path cho `piperead()` sau này, chỉ cần wrap: `if(wq_has_sleeper(&pi->nwrite)) wakeup_one(&pi->nwrite);`

### 5.5. Loại bỏ trường `p->chan` dư thừa

Trong xv6 gốc, `struct proc` có trường `void *chan` để `wakeup()` có thể duyệt và so sánh. Tuy nhiên, với thiết kế wait queue, `chan` đã được lưu bên trong cấu trúc `wq_entry`. Việc giữ lại `p->chan` trong `struct proc` là dư thừa và dễ gây nhầm lẫn.

Cần thực hiện các bước sau để dọn dẹp (clean up):
1. **Trong `kernel/proc.h`**: Bỏ hoàn toàn trường `void *chan;` khỏi `struct proc`.
2. **Trong `kernel/proc.c`**:
   - Trong `sleep()`: Bỏ việc gán `p->chan = chan;` và `p->chan = 0;`.
   - Trong `freeproc()`: Bỏ việc clear `p->chan = 0;`.
3. **Về hàm `kill()`**: Lo ngại ban đầu là `kill()` có thể cần đọc `p->chan` (*ví dụ check `p->chan == &something`*). Nhưng thực tế trên xv6 gốc, `kill()` chỉ kiểm tra `if(p->state == SLEEPING)` để set `p->state = RUNNABLE` chứ không hề phụ thuộc `p->chan`. Do đó, loại bỏ `p->chan` là hoàn toàn an toàn và không gây lỗi trong `kill()`. Việc kiểm tra toàn bộ source kernel (*như `grep p->chan`*) cũng đã xác nhận không còn logic ngầm nào phụ thuộc vào `p->chan`.

### 5.6. Phân bổ hàm theo file

| Hàm | File | Visibility | Ghi chú |
|-----|------|------------|--------|
| `wq_hash()` | `kernel/waitqueue.c` | `static` | Chỉ dùng nội bộ |
| `wq_init()` | `kernel/waitqueue.c` | extern | Gọi từ `main()` |
| `wq_enqueue()` | `kernel/waitqueue.c` | extern | Tự acquire/release bucket lock bên trong. Gọi từ `sleep()` trong `proc.c` |
| `wq_dequeue()` | `kernel/waitqueue.c` | extern | Tự acquire/release bucket lock. Gọi từ `wakeup()` — wake tất cả entry có `chan` trùng |
| `wq_dequeue_one()` | `kernel/waitqueue.c` | extern | Tương tự `wq_dequeue()` nhưng chỉ wake 1 entry đầu tiên. Gọi từ `wakeup_one()` |
| `wq_has_sleeper()` | `kernel/waitqueue.h` | `static inline` | Inline cho fast path, lockless check |
| `sleep()` | `kernel/proc.c` | extern | Giữ nguyên vị trí, gọi `wq_enqueue()` |
| `wakeup()` | `kernel/proc.c` | extern | Giữ nguyên vị trí, gọi `wq_dequeue()` |
| `wakeup_one()` | `kernel/proc.c` | extern | Giữ nguyên vị trí, gọi `wq_dequeue_one()` |

> **Quyết định:** Để `sleep()`/`wakeup()`/`wakeup_one()` ở `proc.c` (giữ tương thích với xv6 codebase), expose `wq_enqueue()`/`wq_dequeue()`/`wq_dequeue_one()` qua `waitqueue.h` cho `proc.c` gọi. Các hàm này tự acquire/release bucket lock bên trong — caller không cần biết về bucket lock.

### 5.7. Nơi cần sửa trong kernel

| File | Sửa gì |
|------|--------|
| `kernel/waitqueue.h` | Định nghĩa struct mới (`wq_entry`, `wq_bucket`, `wq_table`), khai báo extern `wq_enqueue`/`wq_dequeue`/`wq_dequeue_one`/`wq_init`, inline `wq_has_sleeper` (NEW). **Lưu ý:** tên struct thống nhất dùng `wq_` prefix xuyên suốt |
| `kernel/waitqueue.c` | Implement hash table + operations (NEW) |
| `kernel/proc.h` | Thêm `wq_entry` vào `struct proc`, **xoá `void *chan`** |
| `kernel/proc.c` | Rewrite `sleep()`, `wakeup()`, thêm `wakeup_one()`, **xoá các đoạn code liên quan `p->chan`** |
| `kernel/pipe.c` | Dùng `wakeup_one()`, fast path, batch copy |
| `kernel/defs.h` | Khai báo: `void sleep(void*, struct spinlock*)`, `void wakeup(void*)`, `void wakeup_one(void*)`, `void wq_init(void)` |
| `kernel/main.c` | Gọi `wq_init()` trong `main()` trước khi enable scheduling |

---

## 6. Kỳ vọng kết quả (lý thuyết)

> Các con số dưới đây là **kỳ vọng lý thuyết**. Số liệu thực tế cần được đo qua benchmark trên QEMU. Với workload nhẹ, mức cải thiện thực tế sẽ thấp hơn.

| Metric | Xv6 gốc | Sau optimize | Kỳ vọng |
|--------|----------|-------------|---------|
| `wakeup()` scan | O(64) — scan toàn bộ | O(4) — chỉ scan 1 bucket | ~16x lý thuyết |
| Spurious wakeups | Wake tất cả trên channel | Wake 1 (wakeup_one + autoremove) | ~90%+ giảm |
| Context switch thừa | N process bị wake, N-1 ngủ lại | 1 process được wake | Giảm N-1 lần switch |
| Pipe lock hold time | Giữ lock suốt copyin | copyin ngoài lock | Giảm đáng kể |
| Fast path (no sleeper) | Vẫn scan 64 entries | Skip nếu **bucket rỗng** (atomic check) | Phụ thuộc collision (*) |

> (*) **Lưu ý về fast path:** `wq_has_sleeper()` chỉ skip khi toàn bộ bucket rỗng (`head == NULL`). Nếu bucket có entry từ channel khác (do hash collision), fast path vẫn trả về true → vẫn phải gọi `wakeup()` dù channel đích không có ai chờ. Cải thiện thực tế phụ thuộc tỷ lệ collision, không phải luôn 64x.

---

## 7. Tài liệu tham khảo

| Tài liệu | Link |
|-----------|------|
| Linux `include/linux/wait.h` | [Source](https://elixir.bootlin.com/linux/latest/source/include/linux/wait.h) |
| Linux `kernel/sched/wait.c` | [Source](https://elixir.bootlin.com/linux/latest/source/kernel/sched/wait.c) |
| Linux `__wake_up_common()` | [Source](https://elixir.bootlin.com/linux/latest/source/kernel/sched/wait.c) |
| Linux `fs/pipe.c` | [Source](https://elixir.bootlin.com/linux/latest/source/fs/pipe.c) |
| Columbia OS Wait Queues | [Lecture](https://cs4118.github.io/www/2023-1/lect/10-run-wait-queues.html) |
