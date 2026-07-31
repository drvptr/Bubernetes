#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>

#define MAX_EV 32
#define TIMEOUT 2000
#define PERR(msg) do {\
                    fprintf(stderr, "%s:%d:%s:%s: %s\n", __FILE__, __LINE__, __func__, msg, strerror(errno));\
	          } while (0)
#define ERR(msg) do {\
                    fprintf(stderr, "%s:%d:%s: %s\n", __FILE__, __LINE__, __func__, msg);\
	          } while (0)
		  
volatile sig_atomic_t sigterm = 0;

void sigint_handler(int sig){
  (void)sig;
  sigterm = 1;
}

int Init(int epfd);
int Healthcheck(int epfd);
int Func(int epfd);
void CleanUp(int epfd);

int main(void){
  int epfd = epoll_create(MAX_EV);
  if(epfd == -1){
    PERR("epoll_create");
    return epfd;
  }
  if(Init(epfd) != 0){
    ERR("Healthcheck");
  }
  struct epoll_event out_ev[MAX_EV];
  while(!sigterm){
    int n = epoll_wait(epfd, out_ev, MAX_EV, TIMEOUT);
    if(n == -1){
      PERR("epoll_wait");
      break;
    }
    if(n == 0){
      if(Healthcheck(epfd) != 0){
        ERR("Healthcheck");
      }
      continue;
    }
    if(n > 0){
      if(Func(epfd) != 0){
        ERR("Func");
      }
      continue;
    }
  }
  CleanUp(epfd);
  return 0;
}
