# Concurrent_Epoll_Server
Concurrent Bash Server using Epoll and Threadpool

Author: Junchao Chen

Project Feature:

	*DDOS Attack Prevention using timer_fds.
	*Slab Memmory Allocation for Client Structs.
	*Avoid race conditions (as much as possible).
	*Non-Blocking Sockets with Edge Trigger & Oneshot Behavior.
	*Partiral Write Handle, Dynamiclly Allocates Memmory for Partrial Write Buffer.
	*Use of Epoll & Thread Pool, All action on file descriptors are handled by Main Epoll unit.
	
Needs to FIx:
	*Partial Write Handle for protocol exchange
	*Free slave_fd & slave_name 
	*Pointer Arithmetic for free malloced memmory needs to be corrected
