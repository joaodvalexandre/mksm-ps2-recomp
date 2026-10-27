// GOW-Port: procedural wait to recover an own caller from a Windows DLL.
#include <windows.h>
volatile unsigned ticks = 0;
__declspec(noinline) void EsperaPerfilProcedural()
{
    Sleep(10);
    ticks = ticks + 1; // Keep the frame; avoid turning Sleep into a tail jump.
}
int main()
{
    for (unsigned i=0;i<1500;++i) EsperaPerfilProcedural();
    return 0;
}
