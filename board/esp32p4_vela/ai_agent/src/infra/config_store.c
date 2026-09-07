/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "config_store.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include "cJSON.h"

static const char *TAG = "cfgstore";

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── helpers ─────────────────────────────────────────────────── */

static int mkdirs(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
    return OK;
}

static cJSON *load_json(void)
{
    FILE *f = fopen(AGENT_CONFIG_FILE, "r");
    if (!f) return cJSON_CreateObject();

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return cJSON_CreateObject(); }

    char *buf = (char *)malloc((size_t)(sz + 1));
    if (!buf) { fclose(f); return cJSON_CreateObject(); }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root ? root : cJSON_CreateObject();
}

static int save_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (!str) return ERROR;

    /* Use open() with explicit 0600 to ensure config file is owner-only.
     * fopen("w") inherits umask which may be too permissive. */
    int fd = open(AGENT_CONFIG_FILE,
                  O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(str); return ERROR; }

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); free(str); return ERROR; }
    fputs(str, f);
    fclose(f);
    free(str);
    return OK;
}

/* ── public API ──────────────────────────────────────────────── */

int config_store_init(void)
{
    mkdirs(AGENT_DATA_DIR);
    mkdirs(AGENT_CONFIG_DIR);
    mkdirs(AGENT_MEMORY_DIR);
    mkdirs(AGENT_SESSION_DIR);
    syslog(LOG_INFO, "[%s] Config store ready at %s\n", TAG, AGENT_CONFIG_FILE);
    return OK;
}

int claw_config_get(const char *key, char *buf, size_t buf_size)
{
    pthread_mutex_lock(&s_lock);
    cJSON *root = load_json();
    int ret = ERROR;
    if (root) {
        cJSON *item = cJSON_GetObjectItem(root, key);
        if (item && cJSON_IsString(item) && item->valuestring[0] != '\0') {
            strncpy(buf, item->valuestring, buf_size - 1);
            buf[buf_size - 1] = '\0';
            ret = OK;
        }
        cJSON_Delete(root);
    }
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int claw_config_set(const char *key, const char *value)
{
    pthread_mutex_lock(&s_lock);
    cJSON *root = load_json();
    cJSON_DeleteItemFromObject(root, key);
    cJSON_AddStringToObject(root, key, value);
    int ret = save_json(root);
    cJSON_Delete(root);
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int config_del(const char *key)
{
    pthread_mutex_lock(&s_lock);
    cJSON *root = load_json();
    cJSON_DeleteItemFromObject(root, key);
    int ret = save_json(root);
    cJSON_Delete(root);
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int config_erase_all(void)
{
    pthread_mutex_lock(&s_lock);
    cJSON *empty = cJSON_CreateObject();
    int ret = save_json(empty);
    cJSON_Delete(empty);
    pthread_mutex_unlock(&s_lock);
    return ret;
}
