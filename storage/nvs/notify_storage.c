#include "notify_storage.h"
#include "persist.h"

#define NS_NOTIFY       "notify"
#define KEY_NOTIFY_RING "ring"

esp_err_t notify_storage_load(void *buf, size_t *len)
{
    return persist_get_blob(NS_NOTIFY, KEY_NOTIFY_RING, buf, len);
}

esp_err_t notify_storage_save(const void *buf, size_t len)
{
    return persist_set_blob(NS_NOTIFY, KEY_NOTIFY_RING, buf, len);
}
