#include "koncepcja.h"
#ifdef KONCPC_SDL
// On Windows SDL_main.h supplies the WinMain entry point. The SDL-free build
// (KONCPC_MODERN_UI=0) is a console program with a plain main.
#include <SDL3/SDL_main.h>
#endif

int main(int argc, char **argv)
{
  return koncpc_main(argc, argv);
}
