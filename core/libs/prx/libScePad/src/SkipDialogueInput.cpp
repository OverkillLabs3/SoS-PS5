#ifdef _WIN32
#include "prx/libSceSystemService/SkipDialogue.hpp"

// Runs on the input thread for each published Cross press and only flags a request; the dialogue hooks act on it on the game thread.
void SkipDialogueCrossPressed() {
    if (SkipDialogue::Enabled()) SkipDialogue::Request();
}
#endif
