#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H
typedef long LONG; typedef unsigned long ULONG; typedef char *STRPTR; typedef short WORD;
#define TRUE 1
#define FALSE 0
struct Message { int x; };
struct MsgPort { int mp_SigBit; };
struct Library { int x; };
typedef unsigned short UWORD;
#endif
