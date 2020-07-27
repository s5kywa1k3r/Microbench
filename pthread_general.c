#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#ifdef ftrace
    #include <stdarg.h>
    #include <string.h>
    #include <fcntl.h>
    #define _STR(x) #x
    #define STR(x) _STR(x)
    #define MAX_PATH 256
    int trace_fd = -1;
    int trace_en = -1;
    int trace_pid= -1;
#endif

struct timespec *start_point, *end_point;
struct timespec *arrive_point, *run_point;
double * response_time;
unsigned int g_counter;
unsigned int try_count;
unsigned int thread_num;
unsigned int ready_flag = 0;
long ncores;

pthread_mutex_t lock;
pthread_mutex_t condition_lock;
pthread_cond_t cond;

#ifdef ftrace
const char *find_tracefs(void)
{
       static char tracefs[MAX_PATH+1];
       static int tracefs_found;
       char type[100];
       FILE *fp;

       if (tracefs_found)
               return tracefs;

       if ((fp = fopen("/proc/mounts","r")) == NULL) {
               perror("/proc/mounts");
               return NULL;
       }

       while (fscanf(fp, "%*s %"
                     STR(MAX_PATH)
                     "s %99s %*s %*d %*d\n",
                     tracefs, type) == 2) {
               if (strcmp(type, "tracefs") == 0)
                       break;
       }
       fclose(fp);

       if (strcmp(type, "tracefs") != 0) {
               fprintf(stderr, "tracefs not mounted");
               return NULL;
       }

       tracefs_found = 1;

       return tracefs;
}

const char *tracing_file(const char *file_name)
{
       static char trace_file[MAX_PATH+1];
       snprintf(trace_file, MAX_PATH, "%s/%s", find_tracefs(), file_name);
       return trace_file;
}

void trace_write(const char* fmt, ...){
    va_list ap;
    char buf[256];
    int n;

    if(trace_fd < 0){
        // fprintf(stderr, "failed : %d \n", trace_fd);
        return;
    } 
    va_start(ap, fmt);
    n = vsnprintf(buf, 256, fmt, ap);
    va_end(ap);

    write(trace_fd, buf, n);
}
#endif


void increase_counter(int thread_id){

    trace_write("mutex_start");
    pthread_mutex_lock(&lock);
    clock_gettime(CLOCK_MONOTONIC, &run_point[thread_id]);
    response_time[thread_id] += ((run_point[thread_id].tv_sec - arrive_point[thread_id].tv_sec) * 1000 + (double)(run_point[thread_id].tv_nsec - arrive_point[thread_id].tv_nsec) / 1000000);
    g_counter+=1;
    // fprintf(stdout,"[thread_%2d]g_counter : %d ** \n", thread_id, g_counter);
    pthread_mutex_unlock(&lock);
    trace_write("mutex_stop");

}

void* thread_act(void* arg){
    unsigned int l_counter;
    int thread_id = (int)arg;
    int cpu = 0;
    pid_t tid;
    cpu_set_t mask;
    
    tid = syscall(SYS_gettid);
#ifdef core
    cpu = thread_id%ncores+1;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);

    if(sched_setaffinity(tid, sizeof(mask),&mask)){
        exit(1);
    }
    if(sched_setaffinity(tid, sizeof(mask), &mask))
        exit(1);

#endif
#ifdef ftrace
    // struct timespec t;
    // t.tv_sec = 0;
    // t.tv_nsec = 1000;
    // trace_write("before nano");
    // write(trace_fd, "before nano\n", 12);
    // nanosleep(&t, NULL);
    // write(trace_fd, "after nano\n", 11);
    // trace_write("after nano");
    int s;
    char line[64];
    s = sprintf(line, "%d\n", tid);
    write(trace_pid, line, s); 
   
    trace_write("dbg start");
#endif

    pthread_mutex_lock(&condition_lock);
    ready_flag+=1;
    while(ready_flag != thread_num){
        pthread_cond_wait(&cond, &condition_lock);
    }
    pthread_cond_signal(&cond);
        pthread_mutex_unlock(&condition_lock);


    clock_gettime(CLOCK_MONOTONIC, &start_point[thread_id]);
    for(l_counter = 0; l_counter < try_count;l_counter++){
        clock_gettime(CLOCK_MONOTONIC, &arrive_point[thread_id]);
        increase_counter(thread_id);
    }
#ifdef ftrace
    trace_write("dbg done");
#endif 
    clock_gettime(CLOCK_MONOTONIC, &end_point[thread_id]);
    // fprintf(stdout, "debg[%d] : %ld %ld\n",thread_id, end_point[thread_id].tv_sec - start_point[thread_id].tv_sec, end_point[thread_id].tv_nsec - start_point[thread_id].tv_nsec);
    pthread_exit((void*)tid);
}

int main(int argc,char* argv[]){
    pthread_t *p_thread;
    int pthread_id;
    int status;
    double measure = 0;
    pid_t tid;
    cpu_set_t mask;
    struct sched_param param;


    printf("pid : %d\n", getpid());


#ifdef ftrace
    int fd;
    char line[64];
    int s;

    fd = open(tracing_file("current_tracer"), O_WRONLY);
    if (fd < 0)
        exit(-1);
    write(fd, "nop", 3);
    // write(fd, "function", 8);
    write(fd, "function_graph", 14);
    close(fd);

    fprintf(stderr,"trace_marker :  %s\n",tracing_file("trace_marker"));
    trace_fd = open(tracing_file("trace_marker"), O_WRONLY);
    if(trace_fd < 0){
        fprintf(stderr, "trace_fd : failed\n");
    }
    trace_en = open(tracing_file("tracing_on"), O_WRONLY);
    if(trace_en < 0){
        fprintf(stderr, "trace_en : failed\n");
    }
    write(trace_en, "1", 1);
    trace_pid = open(tracing_file("set_ftrace_pid"), O_WRONLY);
    if(trace_pid<0){
        fprintf(stderr, "trace_pid : failed\n");
    }
    write(trace_pid,"-1",1);
    s = sprintf(line, "%d\n", getpid());
    write(trace_pid, line, s); 
#endif
#ifdef core
    ncores = sysconf(_SC_NPROCESSORS_ONLN)-1;
    CPU_ZERO(&mask);
    CPU_SET(0, &mask);    
    tid = syscall(SYS_gettid);
#endif
#ifdef rr
    param.sched_priority = sched_get_priority_max(SCHED_RR);
    if(sched_setscheduler(0,SCHED_RR, &param) == -1){
        printf("failed to set scheduler\n");
        exit(1);
    }
#endif

#ifdef fifo
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if(sched_setscheduler(0,SCHED_FIFO, &param) == -1){
        printf("failed to set scheduler\n");
        exit(1);
    }
#endif

    if(argc!= 3){
        printf("usage : ./pthread_general [$numOfThread] [tryCount]");
        exit(0);
    }

    if(!(atoi(argv[1]) && atoi(argv[2]))){
        printf("[ERROR] argv[1] or [2] is not a positive number (%s, %s)\n", argv[1], argv[2]);
        exit(0);
    }

    try_count = atoi(argv[2]);
    thread_num = atoi(argv[1]);

    //initilization
    pthread_mutex_init(&lock, NULL);
    pthread_mutex_init(&condition_lock, NULL);

    pthread_cond_init(&cond, NULL);
    start_point = (struct timespec*)malloc(sizeof(struct timespec)* thread_num);
    end_point   = (struct timespec*)malloc(sizeof(struct timespec)* thread_num);
    arrive_point = (struct timespec*)malloc(sizeof(struct timespec)* thread_num);
    run_point = (struct timespec*)malloc(sizeof(struct timespec)* thread_num);
    response_time = (double *)malloc(sizeof(double)* thread_num); 
    
    p_thread = (pthread_t*)malloc(sizeof(pthread_t)*thread_num);
    
    for(int i = 0; i < thread_num; i++){
        response_time[i] = 0;
        pthread_id = pthread_create(&p_thread[i], NULL, thread_act, (void*)i);
        if (pthread_id < 0){
            perror("[pthread_create]");
            exit(0);
        }
    }

    for(int i = 0; i < thread_num; i++){
        pthread_join(p_thread[i],(void**)&status);
        measure = ((end_point[i].tv_sec - start_point[i].tv_sec) * 1000 + (double)(end_point[i].tv_nsec - start_point[i].tv_nsec) / 1000000);
        fprintf(stdout,"Thread %3d is Ended with %10lf status : %d aver response time : %10lf realrunning time : %10lf critical Entry ratio : %10lf\n", i,measure,status, response_time[i]/try_count, measure - response_time[i],response_time[i]/measure*100);
    }
#ifdef ftrace
    write(trace_en, "0", 1);
#endif
    if(g_counter != try_count * thread_num){
        printf("g_counter race conditon");
    }

    return 0;
}