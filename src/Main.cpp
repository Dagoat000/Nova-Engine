// File: src/Main.cpp
//
// Entry point. Kept to "construct Application, run it, report fatal
// errors" - everything else lives in Core/ and Graphics/.
#include "Core/Application.h"
#include "Core/Logger.h"
#include <Windows.h>
#include <stdexcept>
#include <string>

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    try
    {
        core::Application app;
        return app.Run();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(std::string("Fatal error: ") + e.what());
        MessageBoxA(nullptr, e.what(), "Engine - Fatal Error", MB_OK | MB_ICONERROR);
        return -1;
    }
}
