#ifndef STUB_proto_dos_h
#define STUB_proto_dos_h
#include <exec/types.h>
#include <dos/dos.h>
struct RDArgs *ReadArgs(STRPTR, LONG *, struct RDArgs *); void FreeArgs(struct RDArgs *); LONG PutStr(STRPTR); LONG Printf(STRPTR, ...); struct DateStamp *DateStamp(struct DateStamp *); void Delay(LONG);
#endif
