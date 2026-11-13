#ifndef STUB_sock
#define STUB_sock
#include <exec/types.h>
#include <sys/select.h>
LONG WaitSelect(LONG, fd_set *, fd_set *, fd_set *, struct timeval *, ULONG *);
#endif
