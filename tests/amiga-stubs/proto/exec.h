#ifndef STUB_proto_exec_h
#define STUB_proto_exec_h
#include <exec/types.h>
ULONG Wait(ULONG); struct Message *GetMsg(struct MsgPort *); void ReplyMsg(struct Message *); struct Library *OpenLibrary(STRPTR, ULONG); void CloseLibrary(struct Library *);
#endif
