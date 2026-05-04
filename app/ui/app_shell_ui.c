#include "app_shell_ui.h"
#include "ui_statusbar.h"

lv_obj_t *app_shell_attach_statusbar(lv_obj_t *screen, bool dark)
{
    return ui_statusbar_create(screen, dark);
}
