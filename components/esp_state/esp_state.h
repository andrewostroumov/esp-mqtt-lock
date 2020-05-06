typedef struct {
    uint32_t free_heap_size;
} esp_state_t;

void esp_state_update(esp_state_t *esp_state);

char *esp_state_serialize(esp_state_t *esp_state);