#include <hal/gpio_types.h>

typedef enum {
    ENV_STATE_LOCK,
    ENV_STATE_UNLOCK,
} env_state_unlock_t;

typedef struct {
    gpio_num_t io;
    env_state_unlock_t unlock;
} env_state_t;

char *env_state_serialize(env_state_t *env_state);
void env_state_deserialize(env_state_t *env_state, char *data);
esp_err_t env_state_apply(env_state_t *state);