#ifndef STUB_intuition_intuition_h
#define STUB_intuition_intuition_h
#include <exec/types.h>
struct Window { struct MsgPort *UserPort; };
struct IntuiMessage { struct Message ExecMessage; ULONG Class; UWORD Code; UWORD Qualifier; };
#define IDCMP_RAWKEY 0x400
#define IDCMP_VANILLAKEY 0x200000
enum { WA_Title=1, WA_Width, WA_Height, WA_MinWidth, WA_MinHeight, WA_MaxWidth, WA_MaxHeight, WA_DragBar, WA_DepthGadget,
 WA_CloseGadget, WA_SizeGadget, WA_Activate, WA_SimpleRefresh, WA_ReportMouse, WA_IDCMP, TAG_DONE };
#endif
