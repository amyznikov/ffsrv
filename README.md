# ffsrv


ffmpeg libavformat/network.c patch:

1) Declare this function pointer somewhere on the top of network.c:   

	int (*ff_poll) (struct pollfd *__fds, nfds_t __nfds, int __timeout) = poll;
	
2) Replace all calls to poll() in this file with call to ff_poll().
They are in functions:

    int ff_network_wait_fd()
    int ff_poll_interrupt()
    
After this, the `ff_poll()` could be easily redirected to application :

```

    int main()
    {
      extern int (*ff_poll)(struct pollfd *__fds, nfds_t __nfds, int __timeout);
      ff_poll = my_own_poll;
    }
    
    int my_own_poll(struct pollfd *__fds, nfds_t __nfds, int __timeout)
    {
    }
```

This trick allows use of cooperative multithreading with ffmpeg network sources.
