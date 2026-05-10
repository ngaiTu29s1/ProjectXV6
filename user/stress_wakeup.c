#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NCHILD 20
#define NROUNDS 50

static void
child_worker(int go_read, int done_write)
{
  char token;

  for(int round = 0; round < NROUNDS; round++){
    if(read(go_read, &token, 1) != 1){
      printf("stress_wakeup: child read failed\n");
      exit(1);
    }
    if(write(done_write, &token, 1) != 1){
      printf("stress_wakeup: child write failed\n");
      exit(1);
    }
  }

  close(go_read);
  close(done_write);
  exit(0);
}

int
main(void)
{
  int go[2];
  int done[2];
  char token = 'x';
  int pid;

  if(pipe(go) < 0 || pipe(done) < 0){
    printf("stress_wakeup: pipe failed\n");
    exit(1);
  }

  for(int i = 0; i < NCHILD; i++){
    pid = fork();
    if(pid < 0){
      printf("stress_wakeup: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      close(go[1]);
      close(done[0]);
      child_worker(go[0], done[1]);
    }
  }

  close(go[0]);
  close(done[1]);

  printf("stress_wakeup: %d children x %d rounds\n", NCHILD, NROUNDS);

  for(int round = 0; round < NROUNDS; round++){
    for(int i = 0; i < NCHILD; i++){
      if(write(go[1], &token, 1) != 1){
        printf("stress_wakeup: parent write failed\n");
        exit(1);
      }
    }
    for(int i = 0; i < NCHILD; i++){
      if(read(done[0], &token, 1) != 1){
        printf("stress_wakeup: parent read failed\n");
        exit(1);
      }
    }
  }

  close(go[1]);
  close(done[0]);

  for(int i = 0; i < NCHILD; i++)
    wait(0);

  printf("stress_wakeup: OK\n");
  exit(0);
}