// Stands in for the emulator's library registration in tests that build one source file on its
// own: the functions are called directly there.
#pragma once
namespace Core::Loader {
class SymbolsResolver;
}
#define LIB_FUNCTION(nid, lib, libversion, mod, function) (void)function; (void)sym;
