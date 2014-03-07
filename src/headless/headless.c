#include <audacious/i18n.h>
#include <audacious/debug.h>
#include <audacious/plugin.h>

static bool_t init (void)
{
    AUDDBG ("Init Headless\n");

    return TRUE;
}

static void cleanup (void)
{
    AUDDBG ("Cleanup Headless\n");
}

AUD_IFACE_PLUGIN
(
    .name = N_("Headless Interface"),
    .init = init,
    .cleanup = cleanup
)
