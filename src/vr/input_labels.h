#pragma once
// Replaces the keyboard-name tokens embedded in tutorial hints with the
// physical VR controls that the supplied bindings use.

namespace trlvr
{
    void input_labels_init();
    // Each frame: pause-menu labels in a level, retail ones in the front end.
    void input_labels_update();
}
