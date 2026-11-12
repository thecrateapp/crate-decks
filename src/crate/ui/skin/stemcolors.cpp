#include "crate/ui/skin/stemcolors.h"

#include "crate/ui/dialogs/dialogstyle.h"

namespace crate {

std::array<QColor, 4> stemColors() {
    const DialogMetrics& variables = DialogMetrics::installed();
    return {
            variables.colour("CrateStemDrums"),
            variables.colour("CrateStemBass"),
            variables.colour("CrateStemOther"),
            variables.colour("CrateStemVocals"),
    };
}

} // namespace crate
