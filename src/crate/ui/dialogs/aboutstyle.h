#pragma once

#include <QString>

class QDialog;

namespace crate {

/// Gives upstream's About (DlgAbout, by the object names of its .ui) the look
/// of the GU1 first-run mockup (plan V2.PR5): no window frame, the logo with
/// "About" and the version as a MonoLabel, the name in display type, the
/// credits, the version rows as MonoLabels and readings, MonoLabel tabs and
/// the actions at the foot. Upstream's credits and licence stay as they are.
void styleAbout(QDialog* pAbout);

} // namespace crate
