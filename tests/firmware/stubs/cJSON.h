#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

cJSON* cJSON_CreateObject(void);
void cJSON_AddStringToObject(cJSON* object, const char* key, const char* value);
void cJSON_AddNumberToObject(cJSON* object, const char* key, double value);
void cJSON_AddBoolToObject(cJSON* object, const char* key, int value);
char* cJSON_PrintUnformatted(const cJSON* object);
void cJSON_Delete(cJSON* object);
void cJSON_free(void* ptr);

#ifdef __cplusplus
}
#endif
