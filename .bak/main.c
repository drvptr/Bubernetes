#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>

/* Maby, I'll move this strings to the another file, in future */
#include <sys/types.h>
#include <sys/socket.h>
#if defined(__FreeBSD__)
  #define AF_INET PF_INET
#endif

#define MAX_EV 32
#define TIMEOUT 2000
#define PERR(msg) do {\
                    fprintf(stderr, "ERROR - %s:%d:%s:%s: %s\n", __FILE__, __LINE__, __func__, msg, strerror(errno));\
	          } while (0)
#define ERR(msg) do {\
                    fprintf(stderr, "ERROR - %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, msg);\
	          } while (0)		  
#define WARN(msg) do {\
                    fprintf(stdout, "WARNING - %s: %s\n", __func__, msg);\
	          } while (0)		  
#define INFO(msg) do {\
                    fprintf(stdout, "INFO - %s: %s\n", __func__, msg);\
	          } while (0)
		  
volatile sig_atomic_t STOP = 0;

void SigHandler(int sig){
  STOP = 1;
}

void SetupSignals(void){
  struct sigaction sigact = { 
    .sa_handler = SigHandler,
    .sa_flags = 0,
  };
  sigemptyset(&sigact.sa_mask);
  sigaction(SIGTERM,&sigact,NULL);
  sigaction(SIGINT,&sigact,NULL);
}

int Init(int epfd){
  int swimfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (swimfd == -1) {
    PERR("Init()");
    return -1;
  }
  INFO("Initialization complete");
  return 0;
}

int PeriodicTasks(int epfd){
  INFO("Someday, whatever will be here");
  return 0;
}

int Run(int epfd, struct epoll_event *events, int n){
  INFO("Working...");
  return 0;
}

void CleanUp(int epfd){
  INFO("The program is successfuly terminated");
}

int main(void){
  SetupSignals();
  int epfd = epoll_create(MAX_EV);
  if(epfd == -1){
    PERR("epoll_create()");
    return epfd;
  }
  if(Init(epfd) != 0){
    ERR("Init()");
  }
  struct epoll_event out_ev[MAX_EV];
  while(!STOP){
    int n = epoll_wait(epfd, out_ev, MAX_EV, TIMEOUT);
    if(n == -1){
      if (errno == EINTR) /* if sigterm */
        continue;
      PERR("epoll_wait()");
      break;
    }
    if(n == 0){
      if(PeriodicTasks(epfd) != 0){
        ERR("PeriodicTasks()");
      }
      continue;
    }
    if(n > 0){
      if(Run(epfd, out_ev, n) != 0){
        ERR("Run()");
      }
      continue;
    }
  }
  CleanUp(epfd);
  return 0;
}
