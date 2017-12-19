#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "tpool.h"

#define TASKS_PER_THREAD 5      

typedef struct tpool{
    int nthreads;
    void(*thread_function)(int);

    int *task_queue;
    int size;
    int count;
    int queue_head;
    int queue_tail;
    int queue_task_number;
    int queue_space_number;

    pthread_mutex_t mtx_queue;
    pthread_mutex_t mtx_tasks;
    pthread_mutex_t mtx_free;

    pthread_cond_t has_tasks;
    pthread_cond_t has_space;

} tpool_t;


static void* thread_loop();
static int dequeue();
static int enqueue(int task);

static tpool_t tpool;
int max_size;

int tpool_init(void (*task)(int)){
    pthread_t thread_id;

    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    max_size = nthreads * TASKS_PER_THREAD;

    if((tpool.task_queue = malloc(sizeof(int) * max_size)) == NULL){
        return -1;}


    tpool.count = 0;
    tpool.queue_head = 0;
    tpool.queue_tail = 0;
    tpool.queue_task_number = 0;
    tpool.queue_space_number = max_size;

    if (pthread_mutex_init(&tpool.mtx_queue, NULL) != 0 ||
        pthread_mutex_init(&tpool.mtx_tasks, NULL) != 0 ||
        pthread_mutex_init(&tpool.mtx_free, NULL) != 0) {
            return -1;}

    if (pthread_cond_init(&tpool.has_tasks, NULL) != 0 || pthread_cond_init(&tpool.has_space, NULL) != 0) {
        return -1;}

    //Retrive task
    tpool.thread_function = task;

    //Loop & Create worker threads
    for(int i = 0; i < nthreads; i++){
	if(pthread_create(&thread_id, NULL, thread_loop, NULL) < 0){
        	return -1;}
	//Detach the thread, so no need for pthread_join
   	if(pthread_detach(thread_id) < 0){
        	return -1;}}
    return 0;
}


static void* thread_loop() {
    while(1) {
        pthread_mutex_lock(&tpool.mtx_tasks);

        while(tpool.queue_task_number == 0){
            pthread_cond_wait(&tpool.has_tasks, &tpool.mtx_tasks);}

        tpool.queue_task_number--;
        pthread_mutex_unlock(&tpool.mtx_tasks);

        int task = dequeue();
        pthread_mutex_lock(&tpool.mtx_free);
        tpool.queue_space_number++;
        pthread_mutex_unlock(&tpool.mtx_free);
        pthread_cond_signal(&tpool.has_space);

	if(task < 0){
		return NULL;}
	tpool.thread_function(task);
    }
    return NULL;
}

static int dequeue() {
    int task;
    pthread_mutex_lock(&tpool.mtx_queue);

    if(tpool.count > 0){
        task = tpool.task_queue[tpool.queue_head];
        tpool.queue_head = (tpool.queue_head + 1) % max_size;
        tpool.count--;}

    pthread_mutex_unlock(&tpool.mtx_queue);
    return task;
}


int tpool_add_task(int task){

    pthread_mutex_lock(&tpool.mtx_free);

    while (tpool.queue_space_number == 0) {
        pthread_cond_wait(&tpool.has_space, &tpool.mtx_free);}

    tpool.queue_space_number--;
    pthread_mutex_unlock(&tpool.mtx_free);

    if(enqueue(task) == -1){
        return -1;}

    pthread_mutex_lock(&tpool.mtx_tasks);
    tpool.queue_task_number++;
    pthread_mutex_unlock(&tpool.mtx_tasks);

    pthread_cond_signal(&tpool.has_tasks);
    return 0;
}

static int enqueue(int task){
    pthread_mutex_lock(&tpool.mtx_queue);
    tpool.task_queue[tpool.queue_tail] = task;
    tpool.queue_tail = (tpool.queue_tail + 1) % max_size;
    tpool.count++;
    pthread_mutex_unlock(&tpool.mtx_queue);

    return 0;
}
