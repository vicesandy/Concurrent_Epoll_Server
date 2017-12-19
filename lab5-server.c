/**************************************************************************************
Author: Junchao Chen

Course: CS591 Adv.Linux

Assignment: Concurrent Bash Server

Project Feature:

	*DDOS Attack Prevention using timer_fds.
	*Slab Memmory Allocation for Client Structs.
	*Avoid race conditions (as much as possible).
	*Non-Blocking Sockets with Edge Trigger & Oneshot Behavior.
	*Partiral Write Handle, Dynamiclly Allocates Memmory for Partrial Write Buffer.
	*Use of Epoll & Thread Pool, All action on file descriptors are handled by Main Epoll unit.

Submitted by 12/15/2017
		
**************************************************************************************/

#define _POSIX_C_SOURCE >= 199309L
#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include "tpool.h"

#define MAX_BUFF 4096
#define MAX_CLIENTS 65535
#define MAX_EVENTS 512

#define PORT 4070
#define SECRET "<cs407rembash>\n"

//Function Prototypes
void server_loop();
void accept_clients();
void handle_timers();
void transfer_data(int source);
void check_secret(int client_fd);
void thread_function(int source_fd);
void handle_bash(char *slave_name);
void remove_client(int client_fd);
void unwritten_buffer_data(int source_fd);

int init_server();
int init_pty(int client_fd); 
int send_protocol(int client_fd);
int rearm_epoll_in(int source_fd);
int rearm_epoll_out(int source_fd);
int delete_from_epoll(int source_fd);
int init_client_struct(int client_fd);
int add_fd_to_epoll(int source_fd, int epoll_fd);

typedef enum {
	NEW, 
	ESTABLISHED, 
	UNWRITTEN, 
	TERMINATED
} Client_Status;

typedef struct client_t{
	int client_fd;
	int master_fd;
	int client_timer_fd;
	int unwritten_data_size;
	char *unwritten;
	Client_Status state;
} Client;

//Server Protocol
const char * rembash = "<rembash>\n";
const char * secret = SECRET;
const char * ok = "<ok>\n";
const char * error = "<error>\n";

//Global Instance
int epoll_fd, epoll_timer_fd, server_fd;
int fd_pairs[MAX_CLIENTS * 2 + 6];
int timer_fd_array[MAX_CLIENTS * 2 + 6];

//Slab Allocation For Clients pointers & Client Structs
Client *client_ptr_array[MAX_CLIENTS * 2 + 6];
Client client_struct_array[MAX_CLIENTS * 2 + 6];

void thread_function(int source_fd){
	if(source_fd == server_fd){ 
		accept_clients();
	}else if(source_fd == epoll_timer_fd){
		handle_timers();
	}else if(client_ptr_array[source_fd] == NULL){
       		 return;}
	else{
		switch(client_ptr_array[source_fd]->state){ 	
			case NEW:
				check_secret(source_fd);
				break;
			case ESTABLISHED:
				transfer_data(source_fd);
				break;
			case UNWRITTEN:
				unwritten_buffer_data(source_fd);
				break;
			case TERMINATED:
				return;}}
}


int init_client_struct(int client_fd){
	client_ptr_array[client_fd] = &client_struct_array[client_fd];
	client_ptr_array[client_fd]-> state = NEW;
	client_ptr_array[client_fd]-> master_fd = -1;
	client_ptr_array[client_fd]-> unwritten_data_size = 0;
	client_ptr_array[client_fd]-> client_timer_fd = -1;
	client_ptr_array[client_fd]-> client_fd = client_fd;

	/*
		Client File Descriptor Epoll Registeration being moved to function send_protocol(), so the write rembash event wont be trigged, an way to improve efficiency of the server(1 less epoll event handle). 

	Also setting client connection socket to blocking until protocoled exachanged is also potential optimation.its NOT implemented in this project

	*/

	return 0;
}

void transfer_data(int source_fd){
	char bash_buffer[MAX_BUFF]; 
	char *read_buffer;
	int nread, nwrite;
	
	//Perform Read
	if((nread = read(source_fd, bash_buffer, MAX_BUFF)) < 0){
		if(errno != EAGAIN){
			remove_client(source_fd);
			return;}}
	
	//Perform Write
	if((nwrite = write(fd_pairs[source_fd], bash_buffer, nread)) < nread){
		//Partial Write Case	
		if(errno != EAGAIN){
			remove_client(source_fd);
			return;}
		else{	
			//De-Register Client COnnection to avoid race condition						
			if(delete_from_epoll(source_fd) == -1){
				fprintf(stderr, "Error De-register client_fd %d", source_fd);}
			
			//reset nwrite incase -1 value which mess with pointer arithmetic
			if(nwrite == -1){
				nwrite = 0;}

			read_buffer = malloc(sizeof(char*) * (nread - nwrite));
			memmove(read_buffer, bash_buffer + nwrite, nread - nwrite);

			client_ptr_array[source_fd]-> unwritten = (read_buffer + nwrite);
			client_ptr_array[source_fd]-> unwritten_data_size = (nread - nwrite);
			client_ptr_array[source_fd]-> state = UNWRITTEN;
			
			if(rearm_epoll_out(source_fd) == -1){
				return;}}
	//All data being written
	}else{
		if(rearm_epoll_in(source_fd) == -1){
				return;}}
}

void unwritten_buffer_data(int source_fd){
	int nread, nwrite;
	nread = client_ptr_array[source_fd]->unwritten_data_size;

	//Perform Write
	if((nwrite = write(fd_pairs[source_fd], client_ptr_array[source_fd]->unwritten, nread)) < nread){
		//Error write();
		if(errno != EAGAIN){
			remove_client(source_fd);
			return;}
		
		//Another Partial Write, reset nwrite incase -1 value which mess with pointer arithmetic
		if(nwrite == -1){
			nwrite = 0;}

		client_ptr_array[source_fd]-> unwritten_data_size = nread - nwrite;
		client_ptr_array[source_fd]-> unwritten += nwrite;
		
		if(rearm_epoll_out(source_fd) == -1){
			return;}
		}else{
		client_ptr_array[source_fd]-> state = ESTABLISHED;
		client_ptr_array[source_fd]-> unwritten_data_size = 0;

		if(client_ptr_array[source_fd]-> unwritten != NULL){		
			free(client_ptr_array[source_fd]-> unwritten);}

		//Add Client Connection Back	
		if((add_fd_to_epoll(source_fd, epoll_fd)) == -1){
			return;}
	}
}


int main(){
   //Init Thread Pool Function
   tpool_init(thread_function);
			
    //Init Server
    if(init_server() == -1){
        fprintf(stderr, "Error init_server()");
        exit(EXIT_FAILURE);}
			
    //Create Main Epool
    if((epoll_fd = epoll_create1(EPOLL_CLOEXEC)) <0) {
        perror("Error Creating Main Epoll");
        exit(EXIT_FAILURE);}
	
    //Create Timer Epoll
    if((epoll_timer_fd = epoll_create1(EPOLL_CLOEXEC)) < 0) {
        perror("Error Creating Timer Epoll");
        exit(EXIT_FAILURE);}

    //Add epoll_timer_fd to main epoll
    if(add_fd_to_epoll(epoll_timer_fd, epoll_fd)){
        fprintf(stderr,"Error Adding Listening Socket to main epoll");
        exit(EXIT_FAILURE);}
	
    //Add server_fd to main epoll
    if(add_fd_to_epoll(server_fd, epoll_fd)){
        fprintf(stderr, "Error Adding Listening Socket to main epoll");
        exit(EXIT_FAILURE);}

	//Call Function To Handle Epoll Selection
	server_loop();

	//Shouldnt get here
	exit(EXIT_FAILURE);
}

int init_server(){
    socklen_t server_len;
    struct sockaddr_in server_address;
    server_len = sizeof(server_address);

    //Step 1: Create Server Socket, Set Socket fd to Close_On_Exec & NON_BLOCK
    if((server_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0){
        perror("Server Socket Create Failed \n");
        return -1;}

    memset(&server_address, '0', server_len); //set all bits to 0
    server_address.sin_family = AF_INET; //address family internet
    server_address.sin_port = htons(PORT); //set port 4070
    server_address.sin_addr.s_addr = htonl(INADDR_ANY); //set ip, replace  inet_addr("127.0.0.1");

    //Step 2: Bind Socket
    if(bind(server_fd, (struct sockaddr *) &server_address, server_len) < 0){
        perror("Bind Socket Failed");
        return -1;}

    //Step 3 Listen Socket
    if(listen(server_fd, 5) < 0){
        perror("Listening Socket Failed");
        return -1;}

    //Reset prot after server termination
    int i = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));

        //Set sig_pipe to be ignored,
    if(signal(SIGPIPE, SIG_IGN) == SIG_ERR){
        perror("Error setting sigpipe ignored");
        return -1;}

    //Set sig_child to be ignored,
    if(signal(SIGCHLD, SIG_IGN) == SIG_ERR){
        perror("Error setting sigchild ignored");
        return -1;}
	
    return 0;
}

void server_loop(){
    int nread;
    struct epoll_event event_list[MAX_EVENTS];
    while(1){
        //Monite main epoll
        nread = epoll_wait(epoll_fd, event_list, MAX_CLIENTS * 2, -1);
        for(int i = 0; i < nread; i++){
            if(event_list[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)){
                remove_client(event_list[i].data.fd);
            }else if(event_list[i].events & (EPOLLIN | EPOLLOUT)){
                tpool_add_task(event_list[i].data.fd);}}
    }
}

int send_protocol(int client_fd){
	int timer_fd;
	struct itimerspec time_spec; //timer_spec

    	if(write(client_fd, rembash, strlen(rembash)) < 0){
		if(errno != EAGAIN){
                        close(client_fd);
		   	return-1;}
		else{
	        	perror("Error writing protocol to client");
	        	return -1;}}

	/* Client Socket fd being added to EPOLL here as previously stated */
	if(add_fd_to_epoll(client_ptr_array[client_fd]-> client_fd, epoll_fd) == -1){
		close(client_fd);
		return -1;}

	//Create Timer_fd
	if((timer_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC)) == -1){
		return -1;}
	
	//Set Up Timer Length
	time_spec.it_value.tv_sec = 10;
	time_spec.it_value.tv_nsec = 0;
	time_spec.it_interval.tv_sec = 0;
	time_spec.it_interval.tv_nsec = 0;
	
	if(timerfd_settime(timer_fd, 0, &time_spec, NULL) < 0){
		perror(":");
        	fprintf(stderr, "Error setting time_fd %d", timer_fd);
		return -1;}

	if(add_fd_to_epoll(timer_fd, epoll_timer_fd)){
		return -1;}

	client_ptr_array[client_fd]-> client_timer_fd = timer_fd;
	timer_fd_array[timer_fd] = client_fd;
	timer_fd_array[client_fd] = timer_fd;

	return 0;
}

void accept_clients(int server_fd){
	int client_fd;
    while((client_fd = accept4(server_fd, (struct sockaddr *) NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) > 0){
		if(client_fd > (MAX_CLIENTS * 2 + 6)){
			fprintf(stderr, "Server Reached Maximum Capacait, try later");
			close(client_fd);	
			return;}

		if(init_client_struct(client_fd) == -1){
			perror("");
			fprintf(stderr, "Error Init Client #%d", (client_fd));
			client_ptr_array[client_fd] = NULL;
			close(client_fd);
			return;}
	
		if(send_protocol(client_fd) == -1){
			perror("");
			fprintf(stderr, "Error Send Protocol to: %d", client_fd);
			close(client_fd);
			client_ptr_array[client_fd] = NULL;
			return;}
		
		//MUST RE-ARM Listening socket after EACH read 
		if(rearm_epoll_in(server_fd) == -1){
			fprintf(stderr, "Error Re-Arming Server_FD");
			exit(EXIT_FAILURE);}}
}

int add_fd_to_epoll(int source_fd, int epoll_fd){
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = source_fd;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, source_fd, &ev) == -1){
        perror(":");
        fprintf(stderr, "Error Adding source fd: %d to epoll %d", source_fd, epoll_fd);
        return -1;}
    return 0;
}

void check_secret(int client_fd){
	int nread;
	char msg_buffer[MAX_BUFF]; //msg_buffer is only used in protocol exchange
	
        //Read & Check secret from client
    if((nread = read(client_fd, msg_buffer, strlen(secret))) >0){
        msg_buffer[nread] = '\0';
        if(strcmp(msg_buffer, secret) != 0){
            write(client_fd, error, strlen(error));//send error to client
            fprintf(stderr, "Client Secret dont match \n");
            remove_client(client_fd);
            return;}}
    if(nread == -1){
	if(errno != EAGAIN){
	   remove_client(client_fd);
	   return;}
	else{
           perror("Error reading SECRET from client");
           return;}}

    //Protocol exchanged, client verified, delete timer from epoll_timer_fd	

	/*Should de-register first but constanly printout annoying error msgs*/

    //Close timer_fd
    close(timer_fd_array[client_fd]);
    //Set Timer fd back to Init value
    client_ptr_array[client_fd]-> client_timer_fd = -1;
	
	//Initialize Client
	if(init_pty(client_fd) == -1){
		return;}
	
	//Rearm client_fd for main epoll
	if(rearm_epoll_in(client_fd) == -1){
		return;}

	client_ptr_array[client_fd]->state = ESTABLISHED;

    //LAST STEP: Send Ok to Client
    if(write(client_fd, ok, strlen(ok)) <0){
        perror("Error writing ok to client");
        return;}
	
}

int init_pty(int client_fd){
    pid_t pid;
    int master_fd;
    char slave_name[MAX_BUFF];

    //Open PTY Master
    if((master_fd = posix_openpt(O_RDWR | O_NOCTTY)) < 0){
        perror("Error on create master");

        return -1;}
	
    //Set master_fd to Close_On_Exec(close all inherited FDs after calling exec)
    if(fcntl(master_fd, F_SETFD, FD_CLOEXEC | O_NONBLOCK) < 0){
        perror("Error on set master_fd Close_on_exec");
        return -1;}

    //Unlock slave for master_fd
    if(unlockpt(master_fd) < 0){
        perror("Error on unlock master");
        return -1;}
	
	//Store slave_name to pass to handle_bash
	strcpy(slave_name, ptsname(master_fd));
	
	//Client & Master File Descriptor mapping client_fd <->master_fd
	client_ptr_array[master_fd] = client_ptr_array[client_fd];
	
	//Store in memmory
	fd_pairs[client_fd] = master_fd;
	fd_pairs[master_fd] = client_fd;

    //Store master_fd into associated client_struct to be used in remove_client();
    client_ptr_array[client_fd] -> master_fd = master_fd;
    //Set Client State to Established
    client_ptr_array[client_fd] -> state = ESTABLISHED;
	
	//Add master_fd to main epoll
	if(add_fd_to_epoll(master_fd, epoll_fd) == -1){
		fprintf(stderr, "Error Adding Master_fd %d to epoll", master_fd);
		remove_client(client_fd);
		return -1;}
	
	//Handle Bash in Child
	switch((pid = fork())){
		case 0:
			handle_bash(slave_name);
			break;
		case -1:
			remove_client(client_fd);
			return -1;}
	return 0;
}


void handle_bash(char *slave_name){
	int slave_fd;
    if(setsid() == -1){
        perror("Error on setsid()");
        exit(EXIT_FAILURE);}

    // Open the slave PTY, Do it here because therefore the slave_fd will Not be opened in Sverer. Avoid blocking signal hangup in epoll.
    if((slave_fd = open(slave_name, O_RDWR)) < 0){
        perror("Error Create Slave");
        exit(EXIT_FAILURE);}

    //Set slave_fd to Close_On_Exec(close all inherited FDs after calling exec)
    if(fcntl(slave_fd, F_SETFD, FD_CLOEXEC) == -1){
        perror("Error on fcntl(slave_fd)");
        close(slave_fd);
        exit(EXIT_FAILURE);}
	
    //Redirect stdin/out/err of Child Process to slave_fd to be read from master
    if(dup2(slave_fd, STDOUT_FILENO) == -1 || dup2(slave_fd, STDERR_FILENO) == -1 || dup2(slave_fd, STDIN_FILENO) == -1){
        perror("error dup child process to slave_fd");
        exit(EXIT_FAILURE);}

        close(slave_fd);

   	 //Call exec
   	 execlp("/bin/bash", "/bin/bash", NULL);

	exit(EXIT_FAILURE);
}

//Only used to temproraly remove client_conncection while perfor partial write
int delete_from_epoll(int source_fd){
  	if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, source_fd, NULL) == -1){
    	perror("");
	fprintf(stderr, "Error Deleting fd %d from epoll", source_fd);
	return -1;
	}
	return 0;
}

void handle_timers(){
	int nread, client_fd;
	struct epoll_event evlist[MAX_EVENTS];
	
	if((nread = epoll_wait(epoll_timer_fd, evlist, MAX_CLIENTS * 2, -1)) > 0){
		for (int i = 0; i < nread; i++){
			//Same mapping as client pty
			client_fd = timer_fd_array[evlist[i].data.fd];
			remove_client(client_fd);}}
	
	if(rearm_epoll_in(epoll_timer_fd) == -1){
		return;}
}

int rearm_epoll_in(int source_fd){
	struct epoll_event ev;
	ev.data.fd = source_fd;
		
	ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, source_fd, &ev) == -1){
		return -1;}
	return 0;
}

int rearm_epoll_out(int source_fd){
	struct epoll_event ev;
	ev.data.fd = source_fd;
		
	ev.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
	if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, source_fd, &ev) == -1){
		return -1;}
	return 0;
}

void remove_client(int source_fd){
	if(client_ptr_array[source_fd] == NULL){
		return;}
	int client_fd = client_ptr_array[source_fd]-> client_fd;
	int master_fd = client_ptr_array[source_fd]-> master_fd;
	int timer_fd  = client_ptr_array[source_fd]-> client_timer_fd;
   	 if(shutdown(client_fd, SHUT_RDWR) == -1) {
        	//perror("Error Perform shutdown(), CLient already closed");
	}

	client_ptr_array[client_fd]->state = TERMINATED;

	if((client_ptr_array[client_fd]-> unwritten) != NULL){
		free(client_ptr_array[client_fd]-> unwritten);}	
	
	client_ptr_array[source_fd] = NULL;
	client_ptr_array[master_fd] = NULL;

	if(client_fd >6){
		close(client_fd);}
	if(master_fd >0){
		close(master_fd);}
	if(timer_fd >0){
		close(timer_fd);}

	return;
}




