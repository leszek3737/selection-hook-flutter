// Relative import to reuse C/ObjC++ sources across all target platforms.
// See the comment in ../selection_hook_flutter.podspec for more information.
// macOS: includes the real C-ABI bridge and ported core (not the stub).
#include "../../src/bridge/c_api_mac.mm"
#include "../../src/mac/selection_hook_core.mm"
#include "../../src/mac/lib/utils.mm"
#include "../../src/mac/lib/clipboard.mm"
#include "../../src/mac/lib/keyboard.mm"
