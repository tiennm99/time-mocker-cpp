#include <Windows.h>

// Exported sentinel — lets the injector verify the DLL was built correctly
extern "C" __declspec(dllexport) DWORD TimeMockerHookVersion()
{
    return 0x0001'0000; // v1.0
}
