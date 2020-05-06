#include <cJSON.h>
#include "esp_system.h"
#include "env_state.h"
#include "string.h"
#include "driver/gpio.h"

#define ENV_STATE_LOCK_DATA "lock"
#define ENV_STATE_UNLOCK_DATA "unlock"

char *env_state_serialize(env_state_t *env_state) {
    if (!env_state) { return NULL; }

    if (env_state->unlock) {
        return ENV_STATE_UNLOCK_DATA;
    } else {
        return ENV_STATE_LOCK_DATA;
    }
}

void env_state_deserialize(env_state_t *env_state, char *data) {
    if (strcmp(data, ENV_STATE_UNLOCK_DATA) == 0) {
        env_state->unlock = ENV_STATE_UNLOCK;
    } else {
        env_state->unlock = ENV_STATE_LOCK;
    }
}

esp_err_t env_state_apply(env_state_t *env_state) {
    return gpio_set_level(env_state->io, env_state->unlock);
}