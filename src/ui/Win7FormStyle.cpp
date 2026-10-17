#include <QResource>

// Keep resource initialization outside the Win7 namespace, matching rcc's
// generated entry point. Direct page tests link the same small asset library.
void initializeWin7FieldResources()
{
    static const bool initialized = [] { Q_INIT_RESOURCE(win7_fields); return true; }();
    Q_UNUSED(initialized);
}
