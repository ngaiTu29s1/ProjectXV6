#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Number of ping-pong round trips per test.
// Each round = parent write → child wakes → child read → child write → parent wakes → parent read
// = 2 sleep/wakeup pairs per round → directly exercises our optimization
#define NROUNDS 1000

static void
pingpong(int rounds)
{
  int p2c[2], c2p[2];
  char buf = 'x';
  int pid, t0, t1, elapsed;

  if(pipe(p2c) < 0 || pipe(c2p) < 0){
    printf("bench_ipc: pipe failed\n");
    exit(1);
  }

  t0 = uptime();

  pid = fork();
  if(pid < 0){
    printf("bench_ipc: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    close(p2c[1]);
    close(c2p[0]);
    for(int i = 0; i < rounds; i++){
      if(read(p2c[0], &buf, 1) != 1) break;
      write(c2p[1], &buf, 1);
    }
    close(p2c[0]);
    close(c2p[1]);
    exit(0);
  }

  close(p2c[0]);
  close(c2p[1]);
  for(int i = 0; i < rounds; i++){
    write(p2c[1], &buf, 1);
    if(read(c2p[0], &buf, 1) != 1) break;
  }
  close(p2c[1]);
  close(c2p[0]);
  wait(0);

  t1 = uptime();
  elapsed = t1 - t0;

  printf("  %d rounds: %d ticks", rounds, elapsed);
  if(elapsed > 0)
    printf(" (%d rounds/tick)\n", rounds / elapsed);
  else
    printf(" (< 1 tick)\n");
}

int
main(void)
{
  printf("bench_ipc: pipe round-trip latency\n");
  printf("  1 round = write + wakeup + read + write + wakeup + read\n");
  printf("\n");

  pingpong(NROUNDS);

  exit(0);
}
