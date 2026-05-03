# Sleep & Wakeup Optimization — xv6-riscv

> **Đề tài 25110** · Tối ưu cơ chế sleep/wakeup trong xv6 kernel · RISC-V

## Tổng quan

Xv6 gốc sử dụng `wakeup()` duyệt **toàn bộ process table O(N)** mỗi lần cần đánh thức process. Project này thay thế bằng **hash-bucketed wait queue O(k)**, lấy cảm hứng từ Linux kernel `wait_queue_head_t`.

```
Trước (xv6 gốc):    wakeup(chan) → scan 64 process → acquire 64 locks
Sau (optimized):     wakeup(chan) → hash → scan ~4 entries → acquire 1 bucket lock
```

## Kiến trúc

```
┌──────────────────────────────────────────────────┐
│             Wait Queue Hash Table (16 buckets)   │
│                                                   │
│  bucket[0]: [lock] → entry → entry → NULL        │
│  bucket[1]: [lock] → NULL                         │
│  ...                                              │
│  bucket[15]: [lock] → entry → NULL                │
└──────────────────────────────────────────────────┘
        ↑                          ↑
   sleep() O(1)              wakeup() O(k)
```

## Kỹ thuật áp dụng (từ Linux kernel)

| Kỹ thuật | Nguồn | Tác dụng |
|----------|-------|----------|
| Hash-bucketed wait queue (16 buckets) | `linux/wait.h` | O(k) thay O(N), k ≈ 4 |
| `prepare_to_wait` pattern | `sched/wait.c` | Không lost wakeup |
| `wakeup_one()` + autoremove | `__wake_up_common()` | Tránh thundering herd |
| `wq_has_sleeper()` fast path | `fs/pipe.c` | Skip wakeup khi bucket rỗng |
| Batch `copyin()` ngoài lock | `pipe_write()` | Giảm IPC latency |

> Chi tiết thiết kế: [docs/design.md](docs/design.md)

## Files thay đổi

| File | Thay đổi | Trạng thái |
|------|----------|------------|
| `kernel/waitqueue.h` | **NEW** — struct definitions | ✅ Done |
| `kernel/waitqueue.c` | **NEW** — hash table + operations | ✅ Done |
| `kernel/proc.h` | Thêm `wq_entry` vào `struct proc` | ✅ Done |
| `kernel/proc.c` | Rewrite `sleep()`, `wakeup()` | ✅ Done — `wakeup_one()` chưa có |
| `kernel/pipe.c` | Fast path + batch copy + `wakeup_one()` | ⏳ TODO |
| `kernel/defs.h` | Khai báo API mới | ✅ Done |
| `kernel/main.c` | Init sequence | ✅ Done |
| `user/bench_ipc.c` | **NEW** — IPC latency benchmark | ⏳ Chưa tạo |
| `user/stress_wakeup.c` | **NEW** — Stress test 20+ process | ⏳ Chưa tạo |

> Tiến độ chi tiết, bugs đang open, và quyết định thiết kế: [docs/progress.md](docs/progress.md)

## Build & Run

### Yêu cầu

- RISC-V cross-compiler: `riscv64-unknown-elf-gcc` hoặc `riscv64-linux-gnu-gcc`
- QEMU: `qemu-system-riscv64`

```bash
# Kiểm tra toolchain
which riscv64-unknown-elf-gcc || which riscv64-linux-gnu-gcc
```

### Build và khởi động QEMU

```bash
# Build sạch + boot (tự detect toolchain)
make clean && make qemu

# Chỉ định toolchain rõ ràng
make TOOLPREFIX=riscv64-unknown-elf- qemu
make TOOLPREFIX=riscv64-linux-gnu-   qemu

# Multi-core (4 CPU)
make CPUS=4 TOOLPREFIX=riscv64-unknown-elf- qemu

# Debug với GDB (mở terminal thứ 2 chạy gdb)
make TOOLPREFIX=riscv64-unknown-elf- qemu-gdb
```

Sau khi boot thành công sẽ thấy prompt:

```
xv6 kernel is booting
$ _
```

### Thoát QEMU

```
Ctrl-A  X
```

### Trong xv6 shell — các lệnh quan trọng

#### Regression tests (bắt buộc pass trước khi merge)

```
$ usertests
```
Chạy toàn bộ test suite (~3248 dòng). Bao gồm: fork, exec, pipe, file I/O, signals, memory.
Kết quả mong đợi: `ALL TESTS PASSED`

```
$ usertests forktest
$ usertests rmdot
```
Chạy một test cụ thể theo tên (tra cứu tên trong `user/usertests.c`).

#### Stress tests

```
$ grind
```
Chạy ngẫu nhiên các syscall trên nhiều process đồng thời. Dùng để phát hiện race condition và deadlock.

```
$ forktest
```
Fork đến giới hạn process table (NPROC=64), kiểm tra cleanup.

```
$ stressfs
```
Stress test filesystem concurrency — nhiều process đọc/ghi file đồng thời.

#### Benchmark (TODO — chưa tạo)

```
$ bench_ipc        # đo IPC latency qua pipe trước/sau optimization
$ stress_wakeup    # stress 20+ process sleep/wakeup đồng thời
```

#### Tiện ích

```
$ ls               # liệt kê file trong thư mục hiện tại
$ cat README       # đọc file
$ echo hello       # in ra stdout
$ grep pattern file
$ wc file          # word count
$ kill pid         # gửi signal tới process
```

### Chạy test tự động từ host (không cần vào shell)

```bash
./test-xv6.py usertests          # chạy usertests tự động
./test-xv6.py -q usertests       # quick subset
./test-xv6.py crash              # crash recovery test
```

## Benchmark — Kỳ vọng lý thuyết

> **Disclaimer:** Số liệu dưới đây là **kỳ vọng lý thuyết trong điều kiện lý tưởng** (tất cả process phân bố đều vào 16 bucket). Thực tế trên QEMU single-core, cải thiện sẽ thấp hơn đáng kể vì overhead context switch vẫn chiếm phần lớn latency. Cần đo benchmark thực tế để xác nhận.

| Metric | Baseline | Optimized | Kỳ vọng |
|--------|----------|-----------|---------|
| `wakeup()` scan | O(64) | O(4) | ~16x lý thuyết |
| Spurious wakeups | Wake all | ≈ 0 | ~90%+ ↓ |
| Context switch thừa | N process bị wake, N-1 ngủ lại | 1 process được wake | Giảm N-1 lần switch |
| Lock contention | 64 × p->lock | 1 bucket lock + k × p->lock | ~16x lý thuyết |
| IPC latency | X ticks | < X ticks | Cần đo thực tế |

## Sprint Plan (7 tuần · 61 pts · 20 cards)

| Sprint | Tuần | Nội dung | Points |
|--------|------|----------|--------|
| 1 | 1 | Setup, đọc codebase | 8 |
| 2 | 2 | Thiết kế & Implement waitqueue | 8 |
| 3 | 3 | Rewrite sleep/wakeup | 8 |
| 4 | 4 | Benchmark cơ bản, usertests, wakeup_one | 8 |
| 5 | 5 | Optimization (pipe), verify subsystems | 9 |
| 6 | 6 | Benchmark so sánh, stress test | 8 |
| 7 | 7 | Fix bugs, báo cáo, demo | 12 |

<details>
<summary>Danh sách 20 cards</summary>

| Card | Task | Sprint | Pts | Trạng thái |
|------|------|--------|-----|------------|
| S1-1 | Clone xv6, boot QEMU | 1 | 2 | ✅ |
| S1-2 | Đọc `proc.c` — hiểu O(N) | 1 | 3 | ✅ |
| S1-3 | Đọc Linux wait queue | 1 | 2 | ✅ |
| S1-4 | Setup git repo + nhánh | 1 | 1 | ✅ |
| S2-1 | Thiết kế `waitqueue.h` + lock ordering | 2 | 3 | ✅ |
| S2-2 | Implement `waitqueue.c` | 2 | 5 | ✅ |
| S3-1 | Rewrite `sleep()` | 3 | 4 | ✅ |
| S3-2 | Rewrite `wakeup()` O(k) | 3 | 4 | ✅ |
| S4-1 | Benchmark `bench_ipc.c` | 4 | 3 | ⏳ |
| S4-2 | Integration test: `usertests` | 4 | 2 | ⏳ |
| S4-3 | `wakeup_one()` | 4 | 3 | ⏳ |
| S5-1 | Pipe fast path + batch copy | 5 | 4 | ⏳ |
| S5-2 | Verify per-bucket lock | 5 | 2 | ⏳ |
| S5-3 | Verify subsystem (pipe, IDE, shell) | 5 | 3 | ⏳ |
| S6-1 | Benchmark so sánh trước/sau | 6 | 5 | ⏳ |
| S6-2 | Stress test 20+ process | 6 | 3 | ⏳ |
| S7-1 | Fix bugs | 7 | 3 | ⏳ |
| S7-2 | Benchmark report | 7 | 3 | ⏳ |
| S7-3 | Báo cáo kỹ thuật 4–6 trang | 7 | 3 | ⏳ |
| S7-4 | Demo script + 2 build | 7 | 3 | ⏳ |

</details>

## Tài liệu tham khảo

- [Linux `wait.h`](https://elixir.bootlin.com/linux/latest/source/include/linux/wait.h) · [Linux `sched/wait.c`](https://elixir.bootlin.com/linux/latest/source/kernel/sched/wait.c) · [Linux `fs/pipe.c`](https://elixir.bootlin.com/linux/latest/source/fs/pipe.c)
- [Columbia OS — Wait Queues Lecture](https://cs4118.github.io/www/2023-1/lect/10-run-wait-queues.html)
